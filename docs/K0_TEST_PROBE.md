# K0: kept connections for the test probe

Status: **for review.** This is slice K0 of
[CONTROL_CONNECTIONS_DESIGN.md](CONTROL_CONNECTIONS_DESIGN.md) section 6,
authorized by cody's PR #132 review ("The next authorized step is K0, subject
to preserving these baseline guarantees and completing independent QA").
K1, K2 and K3 are not started. O1 (a resumed old primary reports `role=p`)
is not started.

## What K0 changes

- **Admin server (`tools/admin_loop.h`, design 4.3).**
  - A connection whose first line is exactly `KEEP 1`, with nothing after
    it, gets `OK keep` and is kept. Requests are `K <seq> <line>`; replies
    are `<seq> <reply>`.
  - `K <seq> MOVE ...` still goes to the MOVE path by prefix.
  - On a kept connection, the worker and the MOVE thread hand the fd back to
    the loop through a queue and an eventfd. They write with a deadline.
  - Idle kept connections live in their own list, never in the half-read set.
    They close after 30 s idle. The cap is the smaller of 64 and
    RLIMIT_NOFILE / 16; past it the least recently used is evicted.
  - On EMFILE nothing changes: no idle kept connection is closed to make
    room for an accept.
  - Bytes after a request line, or waiting before its reply is sent, close
    the connection (protocol error).
  - One-shot connections, HEALTH, STATUS and PROMOTE included, are handled
    exactly as before.
- **`snc_server --no-keep`.** `KEEP 1` is then an unknown request, as on an
  old node. HEALTH appends six counters at the end of its line:
  `adm_kept adm_keep_ok adm_keep_full adm_kept_idle_closed adm_kept_evicted
  adm_kept_proto`.
- **Client `ha::Conn` (`tools/ha.h`, design 4.1 and 4.2).** One kept connection
  to one peer.
  - The handshake runs in a background thread, never on a request's path.
    The request that starts it goes one-shot. After a failed handshake the
    next attempt waits 60 s. After a failed request on a kept connection,
    a new handshake starts at once.
  - Each request checks the idle connection first: older than 20 s, or
    readable (EOF, RST, stray bytes), means close it and go one-shot.
  - A request has `timeout_ms` for the send and `timeout_ms` for the reply,
    with `TCP_USER_TIMEOUT` set. Exactly one reply line with the right
    sequence number is accepted; anything else closes the connection with
    RST.
  - The only resend is one-shot, within the same deadline, and only when the
    verb is read-only (MOVE INFO, MOVE ROLE, MOVE HAVE, READ, PLACEMENT GET),
    the connection was reused, and no reply byte arrived.
  - `SNC_HA_KEEP=0` turns it off: every request is one-shot, no `KEEP 1` is
    sent.
- **Callers.** Only `tests/test_migrate.cpp`'s probe and `node_info` use
  `ha::Conn`. No product caller changes. Every HA-path request stays one-shot
  (checked by `check_oneshot`, below).
- **`migrate::AdminServer` (`tools/migrate.h`).** test_migrate's nodes are its
  own processes serving this thread-per-connection server, not snc_server, so
  it needed the same handshake to be useful to the probe. Its kept loop has
  the same protocol rules, a 30 s idle limit checked against `stop()`, and a
  5 s limit to finish a line. It has no idle cap or handback queue, because it
  is a thread per connection and only test_migrate uses it.

## Tests

- `test_admin_loop` K1 to K7 (server, A4 and A6):
  - K1 requests, including MOVE and a 100 KB reply.
  - K2 protocol errors: wrong prefix, bytes after a line, bytes after a MOVE
    line, a reply holding a newline (the kept connection closes; one-shot
    is unchanged), a second request while the worker holds the first.
  - K3 deadlines from a request's first byte.
  - K4 the idle cap and LRU eviction.
  - K5 A6 with 64 idle kept connections: the worker stalled, the queue full,
    one-shot HEALTH still answered inline; `NO busy` and a partial line on a
    kept connection.
  - K6 keep off. K7 a deterministic stop with kept connections open.
- `test_admin_loop` C1 to C7 (client, against a scripted fake server): normal
  use, an old server, a wrong sequence number, bytes after the reply, idle
  close, a drop on a reused connection (read-only resend, including bare
  `MOVE ROLE`), a late reply (never handed to the next request), keep off.
- `test_admin_loop_keep_off` and `test_migrate_keep_off` run with
  `SNC_HA_KEEP=0`.
- `test_health_baseline.py` adds two scenarios:
  - **kept**: `K HEALTH`, STATUS, MOVE INFO and HEALTH on a real snc_server,
    then a `--no-keep` server answers `KEEP 1` with `NO unknown request`.
  - **emfile_idle**: the EMFILE scenario with 8 kept connections idle on the
    primary (A2(b) "with the idle set populated"). After the fault,
    `adm_kept` is unchanged and `adm_kept_evicted` is 0.
  - `check_oneshot` now also asserts that no HA-path request was kept and
    no `KEEP` was sent on the HA path.

## Baseline correction: the EMFILE figures on main were inflated

While rerunning A2, one K0 EMFILE trial never promoted. The cause was in the
test, not in K0 or main:

- The harness set the primary's soft RLIMIT_NOFILE to its "lowest free
  descriptor", read once from `/proc/<pid>/fd`. If a transient descriptor was
  open at that instant (the node's own 1 s placement poll, for example), the
  limit sat one above a hole. The hole stayed usable, so accept kept
  succeeding now and then, HEALTH kept answering and the witnesses never
  suspected. This was the "possible flake" HEALTH_BASELINE.md finding 6
  recorded.
- **Fix:** `lowest_free_fd` takes the minimum over 100 samples, 1 ms apart.
  After it, K0's EMFILE passed 30 of 30 trials and emfile_idle 15 of 15.
- **Main rerun with the fixed harness** (28733cf, same host, 10 trials,
  `bench/results/health-baseline-20261115/emfile_fixed_harness.json`):
  detection 1028 / 1046 / 1056 ms, promotion 1075 / 1090 / 1106 ms.
- The approved record says 1248 / 1257 / 1273 and 1284 / 1291 / 1333. Those
  trials mostly ran with a hole open, so the primary stayed partly reachable
  and detection waited for a later silent iteration.
- **K0 is judged against the corrected main figures.** It is within
  tolerance against either. The original record is left as it was, with a
  note pointing here. **This is a correction to an approved record and is
  put to cody.**

## A2 rerun (10 trials, same 4 vCPU container, final K0 code)

Raw: `bench/results/k0-20261115/baseline10.json` and `.log`. Times in ms from
the fault, min / median / max. Tolerance (design 5.1): same outcome, and each
max within main's max + 100 ms.

| Scenario | K0 detection | Main detection | K0 promotion | Main promotion | Within |
|---|---|---|---|---|---|
| kill | 1055 / 1075 / 1080 | 1055 / 1069 / 1084 | 1087 / 1120 / 1145 | 1083 / 1111 / 1120 | yes |
| stop | 1245 / 1262 / 1273 | 1238 / 1255 / 1276 | 1270 / 1300 / 1327 | 1266 / 1286 / 1308 | yes |
| partition | 1251 / 1256 / 1278 | 1087 / 1253 / 1272 | 1288 / 1294 / 1315 | 1125 / 1287 / 1308 | yes |
| hold | 1239 / 1262 / 1273 | 1245 / 1268 / 1271 | 1268 / 1309 / 1329 | 1283 / 1308 / 1316 | yes |
| emfile | 1028 / 1050 / 1056 | 1028 / 1046 / 1056 (fixed harness) | 1074 / 1089 / 1099 | 1075 / 1090 / 1106 | yes |
| emfile_idle | 998 / 1031 / 1045 | as emfile | 1036 / 1077 / 1108 | as emfile | yes |
| hold_short | no promotion | no promotion | | | yes |

- Every outcome matches main: every fault trial promoted, the 400 ms hold
  promoted nothing and changed no HA state.
- The largest max increase is kill promotion, +25 ms, inside the 100 ms
  tolerance. The stop scenario's old primary still reports `role=p` after
  SIGCONT (O1, unchanged).
- A2(a): every HA-path request kind is still seen, and all are one-shot.

## test_migrate connections (A1 preview, local, not the hosted A1 run)

SNC_TRACE counts of `ha_ask` lines over the 67 scenarios (both runs passed), keep on against
`SNC_HA_KEEP=0`, same build and host:

| | Keep on | `SNC_HA_KEEP=0` |
|---|---|---|
| one-shot connections answered (`conn=oneshot ok=1`) | 10,967 | 106,152 |
| one-shot attempts that failed (`ok=0`; inferred: mostly connects to a stopped node) | 42,392 | 15,086 |
| handshakes (ok / failed) | 550 / 44 | none |
| requests on kept connections (ok / failed) | 209,073 / 27 | none |
| probe rounds completed | 22,719 | 10,327 |
| run time | 46.7 s | 47.1 s |

- Answered one-shot connections fall 9.7× (106,152 to 11,517 with the
  handshakes counted). The A1 target (10×) is defined on conntrack peaks and
  is measured in K3.
- Failed attempts rise 2.8×: the probe finishes 2.2× more rounds, and in the
  restart scenarios each round still tries the stopped node one-shot.
  Counting every connect attempt, per probe round: 2.4 with keep on against
  11.7 with keep off.
- Raw: `bench/results/k0-20261115/test_migrate_keep1.txt` and `_keep0.txt`.

Hosted A1 (conntrack peaks on 6 runners) is K3's job, not K0's.

## Full suite

60 of 60 passed with keep on, and 60 of 60 with `SNC_HA_KEEP=0` (ctest `-j1`,
same container, final code).

## Independent QA

Three reviewers worked separately: the admin server, the client, and the
tests and scope. None found a blocking defect. The fixes made from their
findings:

- Counters are incremented before the close a client can observe.
- Stray bytes are checked before every kept reply, on the inline, worker,
  MOVE and POLLOUT paths.
- A reply holding a newline is refused on a kept connection (closed, counted
  in `kept_proto`), so the connection can never desynchronize.
- `idle_ms` is clamped to [1, 3600000]; a failed eventfd is traced.
- The client's read-only allowlist matched `MOVE ROLE` only with an argument;
  fixed to match the bare verb too.
- The handshake's connect, send and reply share one 1000 ms budget, and a
  failed `std::thread` creation is caught. `TCP_USER_TIMEOUT` is at least 1.
- The handshake is traced as `conn=handshake` with the verb `KEEP 1`.
- Tests added: the real-server `kept` scenario, `test_migrate_keep_off`, a
  stronger C6 (nothing on the timed-out connection follows the late reply),
  K5's kept `NO busy` and partial line, and a deterministic K7.

## Deviations from the design, put to review

1. **`NO keep full`.** The admin loop refuses `KEEP 1` once live kept
   connections reach twice the idle cap. The design only capped idle ones;
   without this, active kept connections were unbounded.
2. **The read-only resend ships in K0**, not K1, because K0 ships
   `ha::Conn` and the resend is part of its failure handling (design 4.2).
3. **Sequence numbers count per `ha::Conn`, not per connection**, so a number
   never repeats across a reconnect. The server accepts any number and echoes
   it; the check is the client's.
4. **`kept_proto` counts every protocol close:** bad framing, bytes after a
   line, stray bytes before a reply, a MOVE line that failed to finish, and a
   reply holding a newline.
5. **Stray bytes are detectable only before the reply is sent.** Bytes that
   arrive after it are read as the start of the next request and fail its
   framing check, which also closes the connection.
6. **`AdminServer` differs from `admin::Loop`** as described above (no cap, no
   handback queue).
7. **One kept connection per peer, not a pool.** `ha::Pool` with lanes is K1.

## Classification

**REFINE.** New opt-in connection handling in the admin server and a client
used only by the test probe. One-shot handling, HEALTH, the HA path,
failover timing and fencing are unchanged, and the measured timings are
within the 5.1 tolerance.
