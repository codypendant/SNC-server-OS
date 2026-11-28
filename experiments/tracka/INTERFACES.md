# Track A interfaces

The contract between the Track A harness (`tracka/`), the checker (`check/`) and every arm (`arms/<arm>/`). Each
implementer works from this file, `docs/DIRECTION_COMPARISON_PLAN.md` section 3 and
`docs/TRACK_A_EXECUTION_PLAN.md`; nobody reads another's code to learn these. The criteria are frozen in
`bench/results/dc1-tracka/CRITERIA.md`. Times are `time.monotonic_ns()` on one host.

## Scenario

One agent `A`, one task `T`, scenario `S-M` or `S-AP` (from `TRACKA_SCENARIO`).

1. **Model call.** The arm sends one request to the stub and uses its reply.
2. **Policy decision.** The arm applies `experiments/tracka/policy.json` (rule version `p1`). Under `S-M` the rule allows the
   action `T-I,T-F,T-N` without an approval. Under `S-AP` it needs an approval: the arm asks the approver, durably
   waits for the decision, and on `deny` ends `T` with no tool request.
3. **Tools,** in this order: `T-I`, `T-F`, `T-N`.
4. **Memory.** The arm writes agent `A`'s memory and completes `T`.

**Deterministic causality.** Let `rid` be the stub's `request_id` of the reply the arm used. Tool `args` and memory
are exactly:

| Step | Value |
|---|---|
| T-I `args` | `{"reply": rid}` |
| T-F `args` | `{"reply": rid, "ti": <T-I result.effect>}` |
| T-N `args` | `{"reply": rid, "tf": <T-F result.effect>}` |
| memory (approved or S-M) | `{"reply": rid, "ti": <T-I effect>, "tf": <T-F effect>, "tn": <T-N effect, or "owner:happened">}` |
| memory (denied) | `{"reply": rid, "denied": true}` |

`"owner:happened"` is used when the arm could not learn T-N's outcome and the owner answered `happened`.

**Allowed end states:** `T` is `done` or `denied`; or `T` stops (`stopped_in_doubt`) only on a T-N request whose
outcome the arm cannot know, after it asked the owner. An owner answer `happened` lets the arm complete with
`"owner:happened"`; `did_not_happen` lets it send T-N again (once per such answer).

## Environment of every arm command

| Variable | Value |
|---|---|
| `TRACKA_RUN` | the run directory `R` |
| `TRACKA_SVC` | `http://127.0.0.1:<port>`, the shared services |
| `TRACKA_PG` | `postgresql://tracka@127.0.0.1:<port>/tracka`, the run's PostgreSQL 18 (`fsync=on`, `synchronous_commit=on`, no standby); the arm may create databases on it |
| `TRACKA_AGENT`, `TRACKA_TASK` | `A`, `T` |
| `TRACKA_SCENARIO` | `S-M` or `S-AP` |
| `TRACKA_WORKER` | `w1`, `w2`, ... (a new value for every worker process the harness starts) |
| `TRACKA_SEED` | the run's seed |
| `TRACKA_PORTS` | a base port; the arm may use `TRACKA_PORTS` to `TRACKA_PORTS+19` for its own engines |

## Arm manifest: `arms/<arm>/arm.json`

```json
{
  "name": "dbos",
  "engines": [{"name": "temporal", "start": ["..."], "ready": ["..."]}],
  "restart_engine": "pg",
  "setup": ["..."],
  "submit": ["..."],
  "worker": ["..."],
  "deliver": ["..."],
  "state": ["..."],
  "reconstruct": ["..."],
  "config": {"retention": "...", "takeover_timeout_s": 5}
}
```

- Commands are argv lists, run with the arm directory as working directory and the environment above. A path may use
  `{arm}` for the arm directory.
- `engines`: long-running servers the arm needs besides PostgreSQL, started in order after PostgreSQL; `ready` is a
  command that exits 0 when the engine serves. `restart_engine` names the engine restarted at A12 and A13: `pg` for
  library arms, otherwise one of `engines`.
- `setup`: one-shot, after the engines are ready (schemas, deployments, registrations).
- `submit`: one-shot, called once per run, after the first worker is started. It must return once `T` is durably
  submitted. An arm whose worker submits by itself may make it a no-op.
- `worker`: the long-running process the faults act on. The harness starts it in its own process group and kills
  the whole group. Each start is the arm's documented recovery entry point: DBOS restart, a Temporal or Restate
  worker start, a LangGraph re-invoke of the thread, SNC's new epoch. A worker that finishes `T` stays up until the
  harness stops it.
- The harness appends the arguments shown to the command's argv.
- `deliver <task> <action> <decision>`: one-shot. Posts the approver's decision to the arm through the ingress the arm
  chose. It may run while no worker is up, and must return once the decision is durably accepted.
- `state <agent>` and `reconstruct <task>`: one-shot, print one JSON object on stdout (below). They may run while
  workers are down.
- `config`: every timeout or retention value the arm set, reported as is.

## Services (`TRACKA_SVC`)

All requests and replies are JSON. Each service appends to `R/svc/<svc>.jsonl` (one `write` and `fsync` per line)
before it acts. **Every JSONL line in `R`, in every file, carries `t`** (`time.monotonic_ns()`, one host), so order
across files is known.

- **Stub, `POST /stub`** `{invocation_id, retry_of, agent, task, body}`. `invocation_id` is chosen by the arm;
  `body` is any JSON value, the model input. The reply is chunked NDJSON: first `{"request_id": rid}`, then
  `{"output": out, "model": "stub-1"}`. `rid` is fresh for every physical request. Logs `{kind: "request",
  request_id, invocation_id, retry_of, agent, task, body (canonical JSON text), body_sha256}`, then `{kind: "reply",
  request_id, output_sha256}` (`output_sha256` is the sha256 of the UTF-8 `output` string) and `{kind: "delivered", request_id}`.
- **Tools, `POST /T-I`, `/T-F`, `/T-N`** `{call_id, key, attempt, agent, task, token, args}`. `call_id` and
  `attempt` are the arm's ids, used only to correlate. Reply `{kind, result}`:
  - T-I and T-F return the first result again for a repeated `key` (`duplicate`);
  - T-F rejects, with HTTP 409 and `rejected_stale`, a `token` (an integer) below the highest it has seen for that
    agent. The stale check comes before the key dedupe, so a stale token is rejected even under a repeated key. A request without `token` uses its `epoch` field (the frozen SNC arm);
  - T-N has no key and no status query: every request is a new effect;
  - `GET /status?key=` answers T-I's first result for a key, or `{found: false}`.
  Logs `{kind: "request", call_id, key, attempt, agent, task, token, args}`, then `{kind: "effect" | "duplicate" |
  "rejected_stale", call_id, key, attempt, token, result}`, then `{kind: "delivered", call_id, key, attempt}`.
  `result.effect` is the effect id.
- **Approver, `POST /approval`** `{task, action}`. Logs `{kind: "request", task, action}` and replies 202
  `{status: "pending"}`. The harness then runs the arm's `deliver` with decision `approve` on even seeds and `deny`
  on odd seeds, logging `{kind: "posted", task, action, decision, exit}` to `R/svc/approver.jsonl` when `deliver`
  returns. The usual delay before posting is 200 ms.
- **Owner, `GET /owner?task=&call_id=`** answers `{answer: "happened"}` on odd seeds and `did_not_happen` on even
  seeds, and logs `{kind: "answer", task, call_id, answer}` to `R/svc/owner.jsonl`.

## Read answers

`state <agent>`:
```json
{"task": {"id": "T", "status": "done | denied | stopped_in_doubt | running"}, "memory": {...} , "retention": "..."}
```
`memory` is the value above, or `null` when none is written.

`reconstruct <task>`:
```json
{
  "model": [{"invocation_id": "...", "request_id": "...", "input": {...}, "output_sha256": "...", "used": true}],
  "policy": [{"rule_version": "p1", "action": "T-I,T-F,T-N", "result": "allow | needs_approval",
              "approval": {"decision": "approve | deny"} }],
  "calls": [{"tool": "T-I", "call_id": "...", "key": "...", "token": 1, "args": {...},
             "attempts": [{"attempt": 1, "outcome": "effect | duplicate | rejected_stale | unknown | owner:happened | owner:did_not_happen"}],
             "outcome": {"kind": "...", "result": {...}}}],
  "retention": "..."
}
```
- `model[].input` is the `body` sent; the checker compares `sha256(canonical(input))` with the stub's `body_sha256`.
  Canonical JSON is `json.dumps(v, sort_keys=True, separators=(",", ":"), ensure_ascii=False)`.
- `approval` is `null` when the rule needs none. A field the arm has no record for is `null`; the checker scores it
  as unmet for that clause.

## Run directory `R`

| Path | Writer | Content |
|---|---|---|
| `R/run.json` | harness | `{point, seed, seed_string, arm, scenario, kind, boundary, varies, ports, unsupported}`; `unsupported` is copied from `arm.json` (a list of clause ids, empty by default) |
| `R/markers.jsonl` | harness, services | `{t, event, data}`: `reached` (`data: {point, svc, phase: "receipt" | "after_reply" | "approval"}`), `killed`, `started` (`data: {worker}`), `stopped`, `resumed`, `engine-restarted`, `posted`, `quiesced`, `error` (`data: {reason}`, the harness failed), `run-end` (`data: {status: "ended" | "cap" | "error"}`, the harness's own status, not the checker's verdict). In `reached`, `svc` is `model`, `T-I`, `T-F`, `T-N` or `approver` |
| `R/svc/*.jsonl` | services | above |
| `R/answers.jsonl` | harness | `{t, call: "state" | "reconstruct", arg, answer, exit}` after the quiesce |
| `R/logs/<proc>.log` | harness | each process's stdout and stderr |

## Matrix

`docs/DIRECTION_COMPARISON_TRACK_A_MATRIX.csv` (frozen): `point, kind, scenario, boundary, seed, varies`. The run's
seed is the CSV `seed` value `i`, because `varies` (delay, approve or deny, release order) is defined on it. CRITERIA's
`seed(point, i, "tracka:<frozen SHA>")` is the string `tracka:8a0f787304e6b9df2fdf3a2a93f24ab83f34db84:<point>:<i>`,
which keys the stub's outputs: `out = sha256(f"{seed_string}:{rid}:{body_sha256}")`.

| Kind | Boundary | Harness action |
|---|---|---|
| `kill_at_receipt` | `model`, `T-I`, `T-F`, `T-N` | at the arm's first request to that service: log it, `SIGKILL` the worker, then let the reply go to the dead socket; restart a worker |
| `kill_after_reply` | as above | reply, flush, log `delivered`, wait `delay_ms`, `SIGKILL`; restart |
| `kill_at_approval` | `approver` | the approval request is logged, `SIGKILL`, then `deliver`, then restart |
| `event_before_wait` | `approver` | on the request, `deliver` first, then reply 202, then `SIGKILL`; restart |
| `kill_during_recovery` | `T-I_then_first_after_recovery` | as A02, then a second `SIGKILL` at the first service request the recovered worker makes; restart |
| `engine_restart_in_flight` | `T-I`, `T-N` | at receipt, restart `restart_engine` (stop, start, ready), then reply |
| `zombie_takeover` | `model`, `T-I`, `T-F`, `T-N` | at receipt, `SIGSTOP` the worker and start a new one. `zombie_first`: `SIGCONT` the zombie right after the new worker starts. `new_first`: `SIGCONT` it at the first of the new worker's request to the same service, the new worker stopping in doubt, or 60 s. No takeover within 60 s is UNRESOLVED |

The harness polls `state` every 2 s to see an end state, so `state` must be cheap. Only the first request to the
boundary service triggers its fault. Every run ends with the workers up until `T`
reaches an end state or the 5-minute run cap, then the 10 s quiesce, then `state` and `reconstruct`.

## Checker output

`python -m check.check <R>` prints one JSON object and writes it to `R/check.json`:
```json
{"verdict": "pass | violation | unresolved | invalid",
 "guarantees": {"O-1": "pass | fail | unresolved | not_exercised | unsupported", "O-2": "...", "O-3": "...", "O-4": "...", "O-5": "..."},
 "clauses": [{"id": "O-3.ti_once", "guarantee": "O-3", "status": "pass | fail | unresolved | not_exercised | unsupported", "detail": "..."}]}
```
- `invalid`: the run itself is unusable (a service log is missing or cut, `run.json` is missing, the harness reported
  an error in `markers.jsonl`). The harness repeats an invalid run once with the same seed.
- `unresolved`: no clause failed, but a clause could not be decided (for example no takeover within 60 s).
- `unsupported` is set only from `run.json`'s `unsupported` list (for example SNC's approval subclauses), never
  inferred.
- A clause that the run's scenario or point does not exercise is `not_exercised`.
