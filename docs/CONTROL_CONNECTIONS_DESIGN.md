# Persistent control-plane connections: design (2026-11-15, for review)

Status: **design only; authorizes no code.** It is the slice recommended in the PR #129 review
(`docs/handoff/PR129_Connection_Churn_Review_Approval.md`): "design persistent, reusable control-plane
connections, preserving witness liveness detection, fencing, and failover semantics." Background:
`docs/TEST_MIGRATE_CONNECTION_CHURN.md` (C1).

Labels: **Observed** (source, log or counter), **Inferred**, **Open**.

## Summary

1. Every control request is one `ha::ask`: it opens a TCP connection, sends one line, reads one line and
   closes. The servers also close after one reply (`tools/admin_loop.h`, the witness's `serve()`).
   **Observed.**
2. A new count of every `ha::ask` in one untraced local `test_migrate` run (section 2) shows that the
   test's own probe (`MOVE INFO`) makes **81%** of them. The probe made 101,811 of 125,695 asks in 49 s,
   about 2,000 a second. The product's own control traffic in that run was about 24,000: witness Paxos,
   node placement polls, migration steps. **Observed.**
   - This corrects `docs/TEST_MIGRATE_CONNECTION_CHURN.md` 2.1, whose `strace` attribution (probe about a
     quarter) came from a traced run in which the trace itself throttled the probe's tight loop (section 2.3).
3. In production the steady-state churn per witness host is about 30 × G health polls a second for G groups
   (3 witnesses × 10/s each), plus about 5 connections per placement poll per node per second. **Inferred**
   from the code and this count.
4. **Proposed:** an opt-in keep-alive on the existing line protocol. Its rules:
   - the client asks with a `K` prefix, and a server that knows it keeps the connection open after the reply;
   - one request at a time per connection, and no pipelining;
   - a connection is closed on any timeout or error, and never reused after one;
   - the client checks an idle connection before reusing it, and reconnects if the peer closed it;
   - there is no automatic retry after a request has been written.

   Liveness, fencing and failover keep their present meaning: they depend on per-request deadlines and on
   the terms and epochs in the payloads, never on a connection's identity (section 4).
5. **Proposed order of slices** (section 6):
   - K1: library and servers;
   - K2: callers, starting with the hot paths;
   - K3: measurement against the acceptance criteria in section 5.

   Each slice is its own PR, stopped for review.

## 1. Scope

In scope: `ha::ask` and every server that answers it. Those servers are the node admin port (`admin::Loop`),
the witness port (`serve()`, which also carries Paxos and placement), and the test harness's use of them.

Out of scope:
- the replication stream and node links, which are already persistent (`src/core/replication.cpp`,
  `src/core/node_link.cpp`);
- client data connections (`ClusterClient`);
- any change to verbs, payloads, terms, epochs or ballots.

## 2. What opens connections today

### 2.1 Call sites (**Observed**, `grep ha::ask`)

| Caller | Verb(s) | Rate | Deadline |
|---|---|---|---|
| Witness watcher, per group (`snc_witness.cpp` `watch`) | HEALTH (STATUS fallback) to the current primary | every `--poll-ms` (100 ms default) | `poll_ms` |
| Witness watcher, failover | STATUS to the standby, PROMOTE (≤ every 50 ms while promoting) | on failover only | 1000 ms |
| Paxos proposer (`paxos.h` `round`) | READ, PREPARE, ACCEPT, COMMIT to each peer, in parallel threads | per decision and per linear read | per round |
| Engine placement poll (`control.h`) | PLACEMENT GET (each answered with a linear Paxos read) | every `placement_poll_ms` (1000 ms default; 300 in the test) | 2000 ms |
| Engine leases (`control.h`) | LEASES TAKE | on demand | 4000 ms |
| `snc_server` readiness (`ask_witness`) | DEGRADE, NOTREADY, READY, SYNCED | on state changes | 1500 ms |
| `snc_server` start (rejoin, `--adopt-role`) | HEALTH, STATUS, MOVE ROLE to peers | at start only | 500 ms |
| Migration driver (`migrate.h`) | PLACEMENT CAS, MOVE STAGE/EXPORT/SEAL/... | per migration step | 5000 to 15000 ms |
| `test_migrate` probe and `node_info` | MOVE INFO | tight loop, unpaced | 1000 / 2000 ms |

### 2.2 Counted (**Observed**)

`ha::ask` was instrumented to log every call (process, first two words of the verb). The instrumentation is
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

The test's witnesses run placement only (no `--pair`), so this run has no HEALTH polls. Each PLACEMENT GET
cost about 3 READ and 1.5 PREPARE among the witnesses: 8,661 / 2,965 ≈ 2.9 and 4,458 / 2,965 ≈ 1.5.

### 2.3 Correction to the churn document

`docs/TEST_MIGRATE_CONNECTION_CHURN.md` 2.1 attributed about 18k of a traced run's connects to the test and
inferred that the probe makes "about a quarter". Untraced, the probe makes 81%. Under `strace -f`, every
syscall of the probe's tight loop stops for the tracer, which throttles it far more than the paced
callers; the traced run also took 95 s instead of 49 s. So the hosted conntrack load (about 2,000 new
connections a second, about 100k per run) is mostly the probe. **Inferred** from the two counts; the rate
matches (101,811 / 49 s ≈ 2,080 a second).

Consequence: making the probe reuse a connection would remove about 80% of the test's churn by itself. It
would also add headroom to the guard from PR #129, whose 2,000-per-scenario figure would then be far above
need.

## 3. Semantics to preserve

| Property | Today | Where it lives |
|---|---|---|
| **Silence is failure** | A request with no full reply within its deadline returns false; the witness counts that toward `failover_ms` | `ha::ask` deadline; `watch` |
| **No stale replies** | A late reply can never be read as the answer to a later request: each request has its own socket | one connection per request |
| **HEALTH is never behind heavy work** | HEALTH is answered inline on the admin loop; STATUS, PROMOTE and others go in order to one worker; MOVE gets its own thread | `admin_loop.h` (`docs/HEALTH_DECOUPLING.md`) |
| **Bounded server state** | At most `kHalfMax` (256) half-read connections, each with a line due within `kReadMs` (500) of accept; `kQueueMax` heavy requests; `kOutMax` waiting replies | `admin_loop.h` |
| **Fencing** | Terms, epochs, ballots and space UUIDs are in the payloads and checked by the receiver; a connection carries no authority | Paxos, placement CAS, PROMOTE checks |
| **Failover** | The witness decides by Paxos after `failover_ms` of silence; a promoted standby is fenced by term | `watch`, `docs/REPLICATION.md` |
| **Ambiguous outcome** | A request whose reply is lost may or may not have taken effect; every caller already treats false as "unknown" and re-reads state (CAS, Paxos, PROMOTE until it serves) | callers |

## 4. Proposed design

### 4.1 Wire

- **Prefix.** A request may be prefixed by `K ` (keep). A server that supports it answers exactly as for the
  bare request, then keeps the connection open for the next line. A bare request still closes after its
  reply. Old clients are therefore unaffected.
- **Old servers.** A server that does not know the prefix answers `NO unknown request` and closes. The
  client then remembers that peer as one-shot for 60 s and re-sends the request bare on a new connection.
  That re-send is safe: the old server did nothing with a request it did not recognize.
- **One request at a time.** A connection never has more than one request outstanding. Bytes received
  after a request line and before its reply has been sent are a protocol error, and the server closes the
  connection.
- Lines, verbs and replies are unchanged.

### 4.2 Client (`ha::Conn`, `ha::Pool`)

- A pool keyed by (host, port, lane), where the lane is a caller-chosen class: health, control, move or
  probe. HEALTH therefore never queues behind another caller's long request on the same connection. One
  connection per key holds at most one request; a caller wanting concurrency asks on another lane or gets a
  one-shot.
- **Reuse check.** Before sending on an idle connection, the client polls it with a 0 timeout. If the
  connection is readable (EOF, RST or unexpected bytes) or in error, it is closed and replaced. This catches
  every clean close (server idle eviction, restart) before the request is written.
- **Deadline unchanged.** The request's `timeout_ms` covers connect (if needed), send and the full reply,
  as today.
- **Close on any failure.** On timeout, short reply, EOF or error, the connection is closed and never
  reused. This is what keeps "no stale replies": a late reply arrives on a socket nobody reads again.
- **No automatic retry after the request is written.** The caller gets false, exactly as today, and its
  existing "unknown" handling applies.
  - The one retry is the pre-send case: the connection was found dead before any byte was written. Then
    the client reconnects once within the same deadline.
- **Idle limit.** Below the server's idle limit (proposed 20 s against 30 s), so the client usually closes
  first. That puts TIME_WAIT on the client, which is the host that reconnects.
- Thread-safe: a pool entry is checked out for one request and returned after the full reply.

### 4.3 Admin server (`admin::Loop`)

- After a `K` request's reply is fully sent, the fd goes back into the loop's read set as an **idle
  connection** with its own deadline (30 s), instead of being closed.
- Idle connections get their own cap, `kIdleMax` (proposed 512). Past it, the oldest idle one is closed.
  The half-read cap `kHalfMax` and its 500 ms rule then apply only to a connection that has started a line.
- **HEALTH and heavy verbs.** HEALTH is answered inline as now. A heavy verb on a K connection is queued to
  the worker as now; the connection is not read again until its reply is sent. That holds because the
  client never pipelines.
- **MOVE.** It is still handed to its own thread, which reads the rest of the line and answers. For a K
  request the thread then returns the fd to the loop (a small handback queue the loop drains), rather than
  closing it.
  - `MOVE INFO`, the probe's verb, is small, but it cannot be answered inline: `Engine::slot_report` runs a
    task on the slot's shard (`on_shard`, `src/core/engine.cpp`), and the loop must never wait on a shard.
    It stays on the MOVE path, so each probe request still costs a thread start, though no longer a
    connection. **Open:** whether a MOVE thread per connection, kept for the connection's life, is
    preferable; to measure in K1.

### 4.4 Witness server (`serve()`)

- Today: one detached thread per accepted connection, one request, close. With K, the thread loops: read a
  line within the idle deadline, answer, repeat.
- Threads must stay bounded. A cap on live connection threads (proposed 256): past it, a new connection is
  answered `NO busy` and closed. Idle threads exit at the idle deadline.
- Paxos acceptor handlers are unchanged. Each request is still handled whole, under the same locks.

### 4.5 Liveness, fencing, failover

- **Liveness.** A primary that stops answering still fails the watcher's request at `poll_ms`, now on a
  reused connection instead of a new one. The witness counts silence exactly as today.
  - Behavior changes in one case: a primary whose process is alive but whose accept queue is full. Today
    the connect is silent. With an established connection, the request reaches the admin loop if it still
    runs. That is more accurate (HEALTH answers from published state), and it is the purpose of
    HEALTH_DECOUPLING.
  - A killed primary's kernel sends RST or FIN, and the reuse check or the read sees it at once.
  - A dead host gives no FIN; the request times out at `poll_ms`, as today.
- **Fencing.** Unchanged: no request gains authority from its connection, and every term, epoch and ballot
  check stays in the handler. A stale primary that keeps an old connection to a witness is refused by term,
  as it is on a new connection today.
- **Failover.** The watcher's decision path (`failover_ms` of silence, then a Paxos decision, then PROMOTE
  until the standby serves) is unchanged. PROMOTE requests stay one-shot or use the control lane; they are
  rare.
- **Paxos.** Rounds keep their parallel per-peer asks, each on that peer's control-lane connection. A
  round's timeout closes every connection that did not answer, so no late PROMISE or ACCEPTED can be read
  in a later round. Ballot checks are untouched.

### 4.6 What it costs and risks

- **Server memory.** Each idle connection costs one fd and a small buffer on the admin loop, or one thread
  on the witness. With 3 witnesses and 2 lanes per peer, a node holds about 6 idle connections from
  witnesses plus its peers' connections: far below the caps.
- **A half-open connection** (a peer that vanished without RST) costs one failed request at its deadline,
  as today's connect timeout does, and is then closed.
- **Load balancers or NAT** that drop idle flows silently: the reuse check cannot see them; the first
  request after the drop times out. The client's 20 s idle limit keeps idle flows short.
  - **Open:** whether any deployment has middleboxes with shorter idle timeouts.
- **Mixed versions.** Covered by 4.1 (old servers) and by bare requests from old clients.

## 5. Acceptance criteria and measurement

| # | Criterion | How |
|---|---|---|
| A1 | `test_migrate` connections per run fall by at least 10× (conntrack peak from a drained table: from about 100k to under 10k) | hosted, as in the churn document's arm A, 6 runners |
| A2 | No change in failover timing: q10 (`tests/test_baseline_move.py`) and the failover tests pass unchanged, and witness detection of a killed primary is within `failover_ms` + `poll_ms`, as before | `tests/test_async_failover.py`, q10, a new kill test that measures detection time before and after |
| A3 | A stopped (SIGSTOP) or killed peer on a reused connection is detected at the same deadline as on a new one | new deterministic C++ test against `admin::Loop` and the witness |
| A4 | No stale reply: a reply arriving after its request timed out is never returned to a later request | new deterministic test with a server that answers late |
| A5 | Idle eviction, a server restart and an old server (no `K`) each cost at most a reconnect, with no false failure | new tests with each case |
| A6 | HEALTH is never delayed by a heavy request on another lane | extend `tests/test_admin_loop.cpp` |
| A7 | Full suite green; hosted fast and full tiers green | CI |

## 6. Slices (each its own PR, stopped for review)

- **K1, library and servers.** `ha::Conn` and `ha::Pool`, plus `K` support in `admin::Loop` and the
  witness's `serve()`. Tests A3 to A6. No caller changes.
- **K2, callers.** In order of churn:
  1. the `test_migrate` probe and `node_info`;
  2. Paxos peers;
  3. the engine's placement poll;
  4. the witness watcher.

  Each caller is switched behind the pool, and its tests run before the next one is switched.
- **K3, measurement.** Hosted A1 and A2, with numbers before and after. If A1 holds, lower the guard's
  per-scenario figure from PR #129, with evidence.

A smaller first step is possible: switch only the probe once K1 exists, since it alone is 81% of the
test's churn. It is not proposed as a separate slice, because K1 is needed either way.

## 7. Alternatives considered

| Alternative | Why not (or not alone) |
|---|---|
| Close with RST (`SO_LINGER {1, 0}`) | It shortens conntrack's hold to about 10 s but keeps the connect rate. Arm D of the churn document shows a short time-wait alone still filled a 50000 table (Open there). It also changes close behavior for every peer |
| Pace the test's probe | It would cut the test's churn without product change, but loses detection of two serving owners between probes. Not measured; possible as a test-only slice if K1 is not approved |
| Multiplexed protocol (request ids, pipelining) | It is more efficient, but needs request ids in every verb and per-request routing on both ends. Head-of-line and stale-reply rules become per-id. Much larger for no need shown at this rate |
| gRPC or another RPC framework | A new dependency and a rewrite of every verb (Invariant 6: borrow mechanisms, keep SNC's abstractions); not justified by the measured need |

## Classification

**RECONSIDER** (proposed): one TCP connection per control request.

**KEEP**: every verb, payload and fencing check.

**REFINE** (proposed): `ha::ask` gains an opt-in pooled connection, and the admin and witness servers gain
keep-alive.

Nothing is authorized until reviewed.
