# AR-1 interfaces

The contract between the three implementers of `docs/AR1_PREREG.md` (revision 2): the runtime and harness
(`ar1/`), the checker and oracle (`check/`), and the plain DBOS arm (`plain/`). Nobody reads another's code to learn
these; this file is the agreement. Times are `time.monotonic_ns()` (CLOCK_MONOTONIC, one host).

## Run directory `R`

| Path | Writer | Content |
|---|---|---|
| `R/run.json` | harness | `{point, seed, arm: "snc"\|"plain1"\|"plain2", mutation, scenario: "S-M"\|"S-AP", pg: {bin, primary_port, standby_port, primary_data, standby_data}, app_name_prefix}` |
| `R/markers.jsonl` | fault injector, receivers, harness | `{t, event, point, data}` |
| `R/svc/stub.jsonl`, `R/svc/T-I.jsonl`, `R/svc/T-F.jsonl`, `R/svc/T-N.jsonl` | receivers | see below |
| `R/approver.jsonl`, `R/owner.jsonl` | approver, owner | `{t, task, call_id, answer}` |
| `R/gateway.jsonl` | runtime gateway | `{t, call_id \| invocation_id, attempt, bound, tli, flush_lsn, sync_state, decision: "established"\|"waiting"\|"refused"}` |
| `R/cuts.jsonl` | oracle | oracle's own format |
| `R/answers.jsonl` | harness | `{t, call: "state"\|"reconstruct", arg, answer}` from the common read interface |
| `R/blobs/<sha256>` | runtime | content-addressed manifest items |

Every JSONL line is written with one `write` and `fsync` before the writer acts on it.

## Markers (`event`)

- `reached` with `point` = a matrix point name (below): the injector reached the point. Written after the seed's
  block delay, before the harness acts.
- `takeover-waiting`, `takeover-committed`, `post-takeover`, `stale-decision-attempt` (`data: {epoch, decision}`),
  `stale-decision-rejected` (`data: {reason}`), `stale-decision-committed`: the C schedules (prereg 5.1 C).
- `killed`, `restarted`, `standby-stopped`, `standby-resumed`, `released`: harness actions.

Point names: `a1:<commit>`, `a2:<commit>`, `a3:<commit>`, `a4:<commit>`, `b1:<send>`, `b2:<send>`, `b3:<send>`,
where `<commit>` is one of `C-acc I-prep I-disp I-acc I-out P-dec AP-req AP-dec TI-prep TI-disp TI-out TF-prep
TF-disp TF-out TN-prep TN-disp TN-out M-done` (plus recovery commits `TAKEOVER`, `I-unknown`, `TN-indoubt`), and
`<send>` is one of `model T-I T-F T-N`.

## Receiver logs (`R/svc/<svc>.jsonl`)

Before replying, each receiver logs `{t, svc, kind: "request", ...}` and only then acts:

- stub: `request_id, invocation_id, retry_of, agent, epoch, body` (canonical UTF-8 JSON, sorted keys) and
  `body_sha256`; reply `{kind: "reply", invocation_id, request_id, output_sha256}`. `request_id` is fresh and random
  for every physical request, and the output depends on it, so a second physical request is told apart from the first.
- tools: `call_id, key, attempt, agent, epoch, args`; then `{kind: "effect" | "duplicate" | "rejected_stale" |
  "status", call_id, key, result}`. T-I and T-F return the first result for a repeated key (`duplicate`). T-F
  rejects an (agent, epoch) below the highest it has seen for the scope (`rejected_stale`). T-N makes a new effect
  for every request. T-I answers `GET /status?key=` (`status`).
- After the response bytes are flushed to the socket, the receiver logs `{kind: "delivered", invocation_id, request_id}` (stub) or
  `{kind: "delivered", call_id, key, attempt, epoch}` (tools). It means sent, not read by the client.
- Every request entry carries `read: {flush_lsn, sync_state, walsender_pid, tli, error}`: the receiver-side read
  of the primary's `pg_stat_replication` for `application_name = 's1'`, taken on receipt, before logging.

## Runtime database (SNC arm, database `ar1`, schema `snc`)

Every consequential commit inserts one row in `snc.commit_token(token text primary key, xid xid8, epoch int, kind
text, name text, agent text)`. `kind` is `decision`, `outcome`, `takeover`, `cancel` or `recovery`; `name` is the
commit name above. The token is deterministic per workflow and commit, so a re-executed step cannot commit twice.

Current-state rows (one per object, transitions by compare-and-set) and append-only history:

- `snc.agent(id, epoch, version)`; `snc.task(id, agent, status, requester, goal, version)`;
  `snc.memory(id, version, agent, content_hash, token)`;
- `snc.invocation(id, agent, task, epoch, state, manifest, whole_hash, provider_request_id, output, late,
  retry_of, token)`, states `prepared dispatched accepted output result_unknown abandoned`;
- `snc.tool_call(id, agent, task, tool, key, epoch, state, args_hash, attempts, result, policy_id, approval_id,
  token)`, states `prepared dispatched completed rejected in_doubt`; `snc.attempt(call_id, n, epoch, token)`;
- `snc.policy_decision(id, task, call_id, rule_version, inputs_hash, result, obligations, epoch, token)`;
  `snc.approval(id, policy_id, approver, action_hash, decision, token)`;
- `snc.run_event(id, agent, kind, epoch, outstanding, token)`;
- `snc.history(id, kind, obj, from_state, to_state, epoch, token)`.

## Common read interface (both arms)

`python -m <arm_pkg>.iface <dsn> state <agent>` and `... reconstruct <effect>` and `... effects <agent>` print one
JSON object. Both block until what they return is established (package 2.6 d3), or print `{"refused": reason}`.

- `state`: `{agent: {id, epoch, version}, task: {id, status}, memory: [{id, version, content_hash}], calls: {id:
  {state, attempts, result}}, invocations: {id: {state, late, retry_of}}, commits: [{token, xid, epoch, kind,
  name}]}`. `commits` lists every consequential commit visible in the answer's snapshot.
- `reconstruct <effect>` (a tool call id, an invocation id or a memory id): `{shown: {items: [{kind, id, version,
  sha256}], whole_hash}, authorized: {rule_version, approval}, executed: {attempts: [...], states: [...]},
  committed: {transitions: [...]}, commits: [{token, xid, epoch, kind, name}]}`.
- `effects`: `{effects: [ids]}`.

The plain arm may answer a field with `null` where it has no such record; the checker scores that as unmet.

## Environment

`AR1_PGBIN` points at PostgreSQL 18.7 binaries (`setup.sh`). Runs need a non-root user (initdb refuses root).

## DBOS executors

Each app process is a DBOS executor, `e1` for epoch 1 and `e2` for the takeover, with its own system database
`ar1_sys_<executor>` (created by `ar1.pg.Pair`). DBOS 3.6 recovery re-enqueues a dead executor's pending workflows on
a shared internal queue that any live process of the same system database dequeues, so a stale activation's process
could otherwise run the new epoch's workflow. Application connections are named `ar1-<executor>-*`.
