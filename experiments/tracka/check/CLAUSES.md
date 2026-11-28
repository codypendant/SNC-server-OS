# Track A checker clauses

`python -m check.check R` (run from `experiments/tracka/`) writes `R/check.json` in the INTERFACES.md format, plus an
`invalid` list of reasons when the verdict is `invalid`. Sources: `run.json`, `markers.jsonl`, `svc/{stub,T-I,T-F,T-N,
approver,owner}.jsonl`, and the last `state` and `reconstruct` rows of `answers.jsonl`. An answer whose `exit` is not
0, or that is not a JSON object, counts as null. A null or malformed field is unmet for the clause that needs it.

## Terms

- **Tool request**: a `request` line in a tool log. Its outcome is the next unmatched `effect | duplicate |
  rejected_stale` line with the same `(call_id, key, attempt)` (a line whose `token` also matches is preferred), and
  it is **delivered** if a `delivered` line with that triple follows the outcome.
- **Token** of a request: `token`, or `epoch` when `token` is null.
- **Used replies** `U`: the set of `args.reply` over all tool requests except T-F requests answered `rejected_stale`
  (plan 3.3). In a run with no such request, `memory.reply`.
  `u` is the single member of `U`, when there is exactly one.
- **E_ti** and **E_tf**: `result.effect` of the T-I `effect` line and of the T-F `effect` line, when exactly one
  exists.
- **Decision** (S-AP): `approve` on even seeds, `deny` on odd ones. **Deny run**: S-AP with an odd seed.
- **Owner answers** for a call id: the `answer` values in `owner.jsonl` for task `T` and that `call_id`.
- **Time** `t`: every line carries `time.monotonic_ns()` from one host, so "before" and "after" compare `t` across
  files. "Last" and "later" within one log mean log order.
- **Owner answered `a` after `q`**: an owner `answer` line for `q`'s call id with answer `a` and a later `t`.
- **Fault services**: the `svc` of every `reached` marker (`model` read as `stub`).

## Clauses

| Id | Exercised | Pass condition |
|---|---|---|
| `O-1.end_state` | always | `state.task.id` is `T`. Deny run: status `denied`. Otherwise `done` or `stopped_in_doubt`. `stopped_in_doubt` also needs: a T-N request exists; the owner was asked about the last T-N request's call id after that request (either answer); and that request was not delivered, or T-N is a fault service |
| `O-1.one_reply` | always (pass with no tool request outside the stale exemption) | `U` has one member `u`; `u` is the `request_id` of a stub request with a logged `reply` and `delivered`. Tool args without `reply` give `U = {null}` and fail |
| `O-1.no_recall` | always (pass with no use) | No stub request has a `t` after the first tool request whose `args.reply` is a stub `request_id`. No exception: a zombie's and a new worker's concurrent model calls both come before the first downstream use, so they pass on their times |
| `O-1.takeover` | `zombie_takeover` with a `stopped` marker | `unresolved` when the first `resumed` marker is 60 s or more after `stopped`, or missing; otherwise pass |
| `O-2.memory` | always | `state.memory` equals the derived value. Deny run: `{"reply": m, "denied": true}`, where `m` is a delivered stub reply id. `stopped_in_doubt`: `null`. `done`: keys exactly `reply, ti, tf, tn`, with `reply = u`, `ti = E_ti`, `tf = E_tf` (each must exist), and `tn` the effect of the last T-N request if that request has an `effect` and was delivered, or `"owner:happened"` if the owner answered `happened` after it. Any other status fails |
| `O-2.single_exec` | always | Every tool request has agent `A` and task `T`. Deny run: no tool request. Otherwise, leaving out T-F requests answered `rejected_stale`: T-I args are `{"reply": u}`, T-F args `{"reply": u, "ti": E_ti}`, T-N args `{"reply": u, "tf": E_tf}`; all T-I requests share one key, and so do all T-F requests. Repeats of the same request (same key and args) are in the set; T-N repeats are judged by `O-3.tn_no_repeat` |
| `O-3.ti_once` | T-I requested, or a non-deny run | exactly one T-I `effect` line |
| `O-3.tn_no_repeat` | T-N requested, or a non-deny run | For each T-N request after the first, there is an owner `did_not_happen` answer for the previous request's call id, with `t` after the previous request and before this one; each answer licenses one repeat |
| `O-3.tf_stale` | T-F requested, or a non-deny run | Every T-F request has an integer token, and each request, in log order, whose token is below the highest token seen so far for its agent is answered `rejected_stale`, even under a repeated key (T-F checks the token before deduplicating) |
| `O-3.deny_no_tool` | deny run | no request in any tool log |
| `O-4.model` | always | Every stub request matches its own `reconstruct.model` entry: same `invocation_id`, and `request_id` equal or null (an entry with the exact `request_id` is preferred). For each: `sha256(canonical(input)) = body_sha256` (null input fails), and any non-null `output_sha256` equals the logged one. No entry is left over. Does not use `U` |
| `O-4.model_used` | always | For the same pairs (pass if `reconstruct.model` is missing; `O-4.model` reports that): `used` is `true` exactly when the `request_id` is in `U`, and a used entry carries its `request_id` and the logged `output_sha256` |
| `O-4.calls` | always | Every tool request matches its own attempt in a `reconstruct.calls` entry with the same `tool, call_id, key, token, args` and `attempt`. Entries are per call and token, so a resend under a new token must be in its own entry. The attempt outcome is the logged kind; or `unknown` if the request was not delivered, its tool is a fault service, or a later request of the same call follows (same key; any later T-N); or, for T-N, `owner:<a>` where the owner answered `a` for that call id. Attempts that match no request may only be `unknown` or such an `owner:` value. Every entry has attempts. `outcome.kind` is the outcome of the entry's highest attempt, and if that is a logged kind, `outcome.result` equals the logged result |
| `O-4.policy` | always | `reconstruct.policy` has exactly one entry: `rule_version` `p1`, `action` `T-I,T-F,T-N`, `result` `allow` (S-M; `approval` null) or `needs_approval` (S-AP). In S-M this is the ordinary policy decision (SNC's `P-dec`) |
| `O-4.approval` | S-AP | The policy decisions are exactly `[decision]`; `approver.jsonl` holds a request for `T` with that action and a `posted` line with that decision. Deny run: `reconstruct.calls` is `[]` |
| `O-4.retention` | always | `reconstruct.retention` and `state.retention` are non-null and equal |
| `O-5.accepted` | S-AP | The arm asked the approver, only for `T` and the action; at least one decision was posted, and every `deliver` exited 0 |
| `O-5.once` | S-AP | `reconstruct.policy` holds exactly one non-null `approval`, with the decision |
| `O-5.waits` | S-AP (pass with no tool request) | A `posted` line for `T` with the decision exists, and no tool request has a `t` before the first such line |
| `O-5.proceeds` | S-AP | Approve: status `done` or `stopped_in_doubt` and a T-I request exists. Deny: status `denied` |

The clauses that rely on the deterministic-causality table, directly or through `U`, are `O-1.one_reply`,
`O-1.no_recall`, `O-2.memory`, `O-2.single_exec` and `O-4.model_used`. `O-4.model` does not rely on it.

## Run statuses

- **`unsupported`**: only for a clause id listed exactly in `run.json` `unsupported`. Nothing is inferred, and a
  guarantee id such as `O-5` matches no clause. A listed clause is reported `unsupported` whatever it computed. For
  the guarantee roll-up it counts as `not_exercised` if it computed that, otherwise as `unsupported`.
- **Guarantee** = the first of `fail, unresolved, unsupported, pass, not_exercised` among its clauses.
- **Run cap**: when a `run-end` marker has `status: "cap"`, the harness collected no answers, so a failure of a
  clause that reads them (`O-1.end_state`, `O-2.memory`, every `O-4` clause, `O-5.once`, `O-5.proceeds`) is reported
  `unresolved`, and a missing `quiesced` marker or missing answers do not make the run invalid. Failures of the
  log-only clauses stay failures.
- **Verdict**: `invalid` if any reason below holds (clauses are still evaluated and reported, since a violation in an
  invalid run counts); else `violation` if a clause fails; else `unresolved` if one is unresolved; else `pass`.
- **Invalid reasons**: `run.json` missing or unreadable, or a non-integer seed; `markers.jsonl` or `answers.jsonl`
  missing; any line that is not a JSON object, or a file not ending in a newline (cut); a service log missing while a
  marker names that service (`reached`, or `posted` for the approver); an outcome or `delivered` line without its
  request; a line without an integer `t`; a marker `error`, or `run-end` with status `error`; no `quiesced`
  marker; an `answers.jsonl` row less than 10 s after the first `end-state` marker (not checked when there is no
  `end-state`, as in a capped run); no `state` or
  `reconstruct` row; a zombie run with `stopped` but no `resumed`; a posted decision that contradicts the seed; an owner answer that contradicts the truth at its `t`: `happened`
  exactly when T-N's log holds an `effect` line for that call id with an earlier `t`. A missing service log that no marker names counts as empty.

## Readings chosen where plan 3.3 is open

1. Re-call rule: no model request may come after the first downstream request. Zombie runs need no exception, since
   a new worker's call made before the zombie's first downstream use passes on its time.
2. One-reply rule: the reply must have been delivered.
3. `stopped_in_doubt` requires that the owner was asked about the last T-N request after it was sent. Either answer
   is accepted. The T-N outcome must also be one the arm could have missed: the request was not delivered, or T-N
   was a fault boundary.
4. Memory `tn`: the actual effect is accepted only if that reply was delivered; `"owner:happened"` only if the owner
   said so after the last T-N request. Both are accepted when both hold, for example when a zombie learned the
   effect.
5. Single-execution set: request identity is `(key, args)`. T-I and T-F must use one key each; T-N repeats belong to
   O-3.
6. O-4 needs every physical stub request and every tool attempt, including those of killed workers, and one
   reconstruct attempt per logged request. Arms may record before sending. The model `request_id` may be null
   except for the used reply.
7. Exactly one policy decision per task. O-5 "once" means exactly one recorded approval.
8. Waiting is judged against the `posted` line, which the harness writes when `deliver` returns. An arm could act
   between its durable accept and that line only if a worker were up then. At A09 (kill, deliver, restart) and A10
   (deliver, 202, kill, restart) no worker is up then, so a correct arm cannot fail this.
9. Retention must be recorded in both answers, with the same value.
10. Takeover is decided from markers alone: release 60 s or more after the stop counts as no takeover. For
    `zombie_first` the release is immediate, so this clause passes.
11. Within one log, order is log order. Lines are written one at a time with `fsync`, so log order and `t` agree.
