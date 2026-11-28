# Track A checker adequacy (development scale)

| L1 fault | expected | bases | checker result | status |
|---|---|---|---|---|
| approval_deliver_exit_1 | O-5.accepted | 16/16 bases | violation fail: O-5.accepted | flagged |
| approval_never_posted | O-5.accepted | 16/16 bases | violation fail: O-4.approval, O-5.accepted, O-5.waits | flagged |
| approval_posted_after_first_tool | O-5.waits | 7/7 bases | violation fail: O-5.waits | flagged |
| approval_posted_early_for_other_task | O-5.waits | 7/7 bases | violation fail: O-5.waits | flagged |
| approval_requested_other_action | O-5.accepted | 16/16 bases | violation fail: O-5.accepted | flagged |
| approval_requested_other_task | O-5.accepted | 16/16 bases | violation fail: O-5.accepted | flagged |
| deny_memory_reply_not_delivered | O-2.memory | 9/9 bases | violation fail: O-2.memory | flagged |
| doubt_owner_asked_before_last_tn | O-1.end_state | 9/9 bases | violation fail: O-1.end_state, O-4.calls | flagged |
| model_recall_1ns_after_first_use | O-1.no_recall | 65/86 bases | snc-A01-0: unsupported (clause listed in arm.json) (violation fail: O-4.model); snc-A02-0: unsupported (clause listed in arm.json) (violation fail: O-4.model); +19 more | flagged |
| model_recall_after_all_tools | O-1.no_recall | 65/86 bases | snc-A01-0: unsupported (clause listed in arm.json) (violation fail: O-4.model); snc-A02-0: unsupported (clause listed in arm.json) (violation fail: O-4.model); +19 more | flagged |
| one_reply_no_reply_line | O-1.one_reply | 65/65 bases | violation fail: O-1.one_reply, O-4.model, O-4.model_used | flagged |
| one_reply_not_delivered | O-1.one_reply | 65/65 bases | violation fail: O-1.one_reply | flagged |
| one_reply_two_reply_ids | O-1.one_reply | 65/86 bases | snc-A01-0: unsupported (clause listed in arm.json) (violation fail: O-4.calls, O-4.model); snc-A02-0: unsupported (clause listed in arm.json) (violation fail: O-4.calls, O-4.model); +19 more | flagged |
| owner_happened_answer_removed | O-2.memory | 9/9 bases | violation fail: O-2.memory, O-4.calls | flagged |
| posted_decision_contradicts_seed | invalid=True | 16/16 bases | invalid fail: O-4.approval, O-5.waits invalid: ['approver posted deny, seed says approve'] | flagged |
| takeover_after_61s | unresolved=['O-1.takeover'] | 28/28 bases | unresolved unresolved: O-1.takeover | flagged |
| tf_effect_id_differs_from_memory | O-2.memory | 65/86 bases | snc-A01-0: unsupported (clause listed in arm.json) (violation fail: O-4.calls); snc-A02-0: unsupported (clause listed in arm.json) (violation fail: O-4.calls); +19 more | flagged |
| tf_request_other_task | O-2.single_exec | 65/86 bases | snc-A01-0: unsupported (clause listed in arm.json) (violation fail: O-4.calls); snc-A02-0: unsupported (clause listed in arm.json) (violation fail: O-4.calls); +19 more | flagged |
| tf_request_other_ti_arg | O-2.single_exec | 65/86 bases | snc-A01-0: unsupported (clause listed in arm.json) (violation fail: O-4.calls); snc-A02-0: unsupported (clause listed in arm.json) (violation fail: O-4.calls); +19 more | flagged |
| tf_stale_epoch_accepted | O-3.tf_stale | 86/86 bases | violation fail: O-3.tf_stale, O-4.calls | flagged |
| tf_stale_token_accepted_as_duplicate | O-3.tf_stale | 86/86 bases | violation fail: O-3.tf_stale, O-4.calls | flagged |
| tf_stale_token_accepted_as_effect | O-3.tf_stale | 86/86 bases | violation fail: O-2.memory, O-2.single_exec, O-3.tf_stale, O-4.calls | flagged |
| tf_token_not_integer | O-3.tf_stale | 86/86 bases | violation fail: O-3.tf_stale, O-4.calls | flagged |
| ti_duplicate_logged_as_effect | O-3.ti_once | 6/6 bases | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once, O-4.calls | flagged |
| ti_logged_before_model_request | O-1.no_recall | 65/86 bases | snc-A01-0: unsupported (clause listed in arm.json) (pass); snc-A02-0: unsupported (clause listed in arm.json) (pass); +19 more | flagged |
| ti_request_after_deny | O-3.deny_no_tool | 9/9 bases | violation fail: O-2.single_exec, O-3.deny_no_tool, O-4.calls | flagged |
| ti_request_extra_arg | O-2.single_exec | 65/86 bases | snc-A01-0: unsupported (clause listed in arm.json) (violation fail: O-4.calls); snc-A02-0: unsupported (clause listed in arm.json) (violation fail: O-4.calls); +19 more | flagged |
| ti_request_other_agent | O-2.single_exec | 65/86 bases | snc-A01-0: unsupported (clause listed in arm.json) (violation fail: O-4.calls); snc-A02-0: unsupported (clause listed in arm.json) (violation fail: O-4.calls); +19 more | flagged |
| ti_request_second_key | O-2.single_exec | 65/86 bases | snc-A01-0: unsupported (clause listed in arm.json) (violation fail: O-4.calls); snc-A02-0: unsupported (clause listed in arm.json) (violation fail: O-4.calls); +19 more | flagged |
| ti_second_effect | O-3.ti_once | 86/86 bases | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once, O-4.calls | flagged |
| ti_zero_effects | O-3.ti_once | 86/86 bases | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once, O-4.calls | flagged |
| tn_effect_not_delivered | O-2.memory | 56/56 bases | violation fail: O-2.memory | flagged |
| tn_memory_stale_after_licensed_repeat | O-2.memory | 64/84 bases | snc-A01-0: unsupported (clause listed in arm.json) (violation fail: O-4.calls); snc-A02-0: unsupported (clause listed in arm.json) (violation fail: O-4.calls); +18 more | flagged |
| tn_one_dnh_two_repeats | O-3.tn_no_repeat | 84/84 bases | violation fail: O-2.memory, O-3.tn_no_repeat, O-4.calls | flagged |
| tn_repeat_after_owner_happened | O-3.tn_no_repeat | 86/86 bases | violation fail: O-2.memory, O-3.tn_no_repeat, O-4.calls | flagged |
| tn_repeat_dnh_before_previous_request | O-3.tn_no_repeat | 84/84 bases | violation fail: O-2.memory, O-3.tn_no_repeat, O-4.calls | flagged |
| tn_repeat_dnh_for_other_call_id | O-3.tn_no_repeat | 84/84 bases | violation fail: O-2.memory, O-3.tn_no_repeat, O-4.calls | flagged |
| tn_repeat_no_owner_answer | O-3.tn_no_repeat | 86/86 bases | violation fail: O-2.memory, O-3.tn_no_repeat, O-4.calls | flagged |
| tn_request_after_deny | O-3.deny_no_tool | 9/9 bases | violation fail: O-2.single_exec, O-3.deny_no_tool, O-4.calls | flagged |
| tn_request_wrong_tf_arg | O-2.single_exec | 65/86 bases | snc-A01-0: unsupported (clause listed in arm.json) (violation fail: O-4.calls); snc-A02-0: unsupported (clause listed in arm.json) (violation fail: O-4.calls); +19 more | flagged |
| negative_doubt_owner_asked_after_tn | must_not=['O-1.end_state', 'O-2.memory'] | 9/9 bases | pass | correct-pass |
| negative_stale_tf_rejected_other_reply | must_not=['O-2.single_exec', 'O-3.tf_stale', 'O-1.one_reply'] | 65/65 bases | violation fail: O-4.calls | correct-pass |
| negative_stale_tf_rejected_other_ti | must_not=['O-2.single_exec', 'O-3.tf_stale'] | 65/65 bases | violation fail: O-4.calls | correct-pass |
| negative_tn_one_dnh_one_repeat | must_not=['O-3.tn_no_repeat'] | 84/84 bases | violation fail: O-2.memory, O-4.calls | correct-pass |
| negative_unchanged | clean=True | 95/95 bases | pass | correct-pass |

| L2 fault (dbos) | run | expected | checker result | proxy evidence | status |
|---|---|---|---|---|---|
| deliver_drop | A09/S-AP/0 | O-5.proceeds | unresolved unresolved: O-1.end_state, O-2.memory, O-4.approval, O-4.calls, O-4.model, O-4.model_used, O-4.policy, O-4.retention, O-5.once, O-5.proceeds | proxy applied 1x, errors 0 | unresolved (cap) |
| approval_flip | A09/S-AP/0 | O-5.once, O-4.approval | violation fail: O-4.approval, O-5.once | proxy applied 1x, errors 0 | flagged |
| approval_flip | A09/S-AP/1 | O-5.once, O-4.approval | violation fail: O-4.approval, O-5.once | proxy applied 1x, errors 0 | flagged |
| approval_null | A09/S-AP/0 | O-5.once, O-4.approval | violation fail: O-4.approval, O-5.once | proxy applied 1x, errors 0 | flagged |
| approval_null | A09/S-AP/1 | O-5.once, O-4.approval | violation fail: O-4.approval, O-5.once | proxy applied 1x, errors 0 | flagged |
| approval_without_event | none/S-M/0 | O-4.policy | violation fail: O-4.policy | proxy applied 1x, errors 0 | flagged |
| attempt_drop | none/S-M/0 | O-4.calls | violation fail: O-4.calls | proxy applied 1x, errors 0 | flagged |
| attempt_drop | A04/S-M/0 | O-4.calls | violation fail: O-4.calls | proxy applied 1x, errors 0 | flagged |
| attempt_outcome | none/S-M/0 | O-4.calls | violation fail: O-4.calls | proxy applied 1x, errors 0 | flagged |
| call_args | none/S-M/0 | O-4.calls | violation fail: O-4.calls | proxy applied 1x, errors 0 | flagged |
| call_drop | none/S-M/0 | O-4.calls | violation fail: O-4.calls | proxy applied 1x, errors 0 | flagged |
| call_token | none/S-M/0 | O-4.calls | violation fail: O-4.calls | proxy applied 1x, errors 0 | flagged |
| deliver_drop_fail | A09/S-AP/1 | O-5.accepted | violation fail: O-5.accepted unresolved: O-1.end_state, O-2.memory, O-4.approval, O-4.calls, O-4.model, O-4.model_used, O-4.policy, O-4.retention, O-5.once, O-5.proceeds | proxy applied 1x, errors 0 | flagged (capped run) |
| deliver_flip | A09/S-AP/0 | O-5.proceeds | violation fail: O-1.end_state, O-2.memory, O-4.approval, O-5.once, O-5.proceeds | proxy applied 2x, errors 0 | flagged |
| deliver_flip | A09/S-AP/1 | O-5.proceeds | violation fail: O-1.end_state, O-2.memory, O-2.single_exec, O-3.deny_no_tool, O-4.approval, O-5.once, O-5.proceeds | proxy applied 2x, errors 0 | flagged |
| deliver_flip | A10/S-AP/1 | O-5.proceeds | violation fail: O-1.end_state, O-2.memory, O-2.single_exec, O-3.deny_no_tool, O-4.approval, O-5.once, O-5.proceeds | proxy applied 1x, errors 0 | flagged |
| mem_drop_tn | none/S-M/0 | O-2.memory | violation fail: O-2.memory | proxy applied 7x, errors 1 | flagged |
| mem_extra_key | none/S-M/0 | O-2.memory | violation fail: O-2.memory | proxy applied 7x, errors 2 | flagged |
| mem_reply | none/S-M/0 | O-2.memory | violation fail: O-2.memory | proxy applied 7x, errors 2 | flagged |
| mem_tf | none/S-M/0 | O-2.memory | violation fail: O-2.memory | proxy applied 7x, errors 2 | flagged |
| model_drop | none/S-M/0 | O-4.model | violation fail: O-4.model | proxy applied 1x, errors 0 | flagged |
| model_extra | none/S-M/0 | O-4.model | violation fail: O-4.model | proxy applied 1x, errors 0 | flagged |
| model_input | none/S-M/0 | O-4.model | violation fail: O-4.model | proxy applied 1x, errors 0 | flagged |
| model_output_sha | none/S-M/0 | O-4.model | violation fail: O-4.model, O-4.model_used | proxy applied 1x, errors 0 | flagged |
| model_used_false | none/S-M/0 | O-4.model_used | violation fail: O-4.model_used | proxy applied 1x, errors 0 | flagged |
| outcome_result | none/S-M/0 | O-4.calls | violation fail: O-4.calls | proxy applied 1x, errors 0 | flagged |
| policy_action | none/S-M/0 | O-4.policy | violation fail: O-4.policy | proxy applied 1x, errors 0 | flagged |
| policy_drop | none/S-M/0 | O-4.policy | violation fail: O-4.policy | proxy applied 1x, errors 0 | flagged |
| policy_dup | none/S-M/0 | O-4.policy | violation fail: O-4.policy | proxy applied 1x, errors 0 | flagged |
| policy_dup | A09/S-AP/0 | O-5.once | violation fail: O-4.approval, O-4.policy, O-5.once | proxy applied 1x, errors 0 | flagged |
| policy_dup | A09/S-AP/1 | O-5.once | violation fail: O-4.approval, O-4.policy, O-5.once | proxy applied 1x, errors 0 | flagged |
| policy_result | none/S-M/0 | O-4.policy | violation fail: O-4.policy | proxy applied 1x, errors 0 | flagged |
| recon_retention | none/S-M/0 | O-4.retention | violation fail: O-4.retention | proxy applied 1x, errors 0 | flagged |
| rule_version_null | none/S-M/0 | O-4.policy | violation fail: O-4.policy | proxy applied 1x, errors 0 | flagged |
| rule_version_null | A09/S-AP/0 | O-4.policy | violation fail: O-4.policy | proxy applied 1x, errors 0 | flagged |
| state_retention | none/S-M/0 | O-4.retention | violation fail: O-4.retention | proxy applied 9x, errors 0 | flagged |
| state_task_id | none/S-M/0 | O-1.end_state | violation fail: O-1.end_state | proxy applied 9x, errors 0 | flagged |
| status_done_to_doubt | none/S-M/0 | O-1.end_state | violation fail: O-1.end_state, O-2.memory | proxy applied 7x, errors 0 | flagged |
| deliver_twice | A09/S-AP/0 | none (must pass) | pass | proxy applied 2x, errors 0 | correct-pass |
| deliver_twice | A10/S-AP/1 | none (must pass) | pass | proxy applied 1x, errors 0 | correct-pass |
| passthrough | none/S-M/0 | none (must pass) | pass | proxy applied 0x, errors 0 | correct-pass |
| passthrough | A04/S-M/0 | none (must pass) | pass | proxy applied 0x, errors 0 | correct-pass |
| passthrough | A09/S-AP/1 | none (must pass) | pass | proxy applied 0x, errors 0 | correct-pass |
| passthrough | A10/S-AP/0 | none (must pass) | pass | proxy applied 0x, errors 0 | correct-pass |

| control (dbos, unmodified) | checker result | harness | detail | status |
|---|---|---|---|---|
| A01-0 | violation fail: O-4.model | ended | - | step-1 finding |
| A02-0 | violation fail: O-4.calls | ended | - | step-1 finding |
| A02-1 | violation fail: O-4.calls | ended | - | step-1 finding |
| A03-0 | violation fail: O-4.calls | ended | - | step-1 finding |
| A04-0 | pass | ended | - | pass |
| A04-1 | pass | ended | - | pass |
| A05-0 | violation fail: O-4.model | ended | - | step-1 finding |
| A06-0 | violation fail: O-4.calls | ended | - | step-1 finding |
| A06-1 | pass | ended | - | pass |
| A07-0 | violation fail: O-4.calls | ended | - | step-1 finding |
| A08-0 | pass | ended | - | pass |
| A08-1 | pass | ended | - | pass |
| A09-0 | pass | ended | - | pass |
| A09-1 | pass | ended | - | pass |
| A10-0 | pass | ended | - | pass |
| A10-1 | pass | ended | - | pass |
| A11-0 | violation fail: O-4.calls | ended | - | step-1 finding |
| A12-0 | pass | ended | - | pass |
| A13-0 | pass | ended | - | pass |
| A14-0 | violation fail: O-4.model | ended | - | step-1 finding |
| A14-1 | pass | ended | - | pass |
| A15-0 | violation fail: O-4.calls | ended | - | step-1 finding |
| A15-1 | pass | ended | - | pass |
| A16-0 | violation fail: O-4.calls | ended | - | step-1 finding |
| A16-1 | pass | ended | - | pass |
| A17-0 | pass | ended | - | pass |
| A17-1 | pass | ended | - | pass |
| none-S-M-0 | pass | ended | - | pass |

| L3 mutant (dbos) | run | expected | checker result | mutation evidence | control fails | status |
|---|---|---|---|---|---|---|
| O-1-model-not-durable | none-S-M-0 | O-1 (new clause) | violation fail: O-4.model | model-call marks 1, stub requests after first use 0 | - | unresolved (not exercised) |
| O-1-model-not-durable | A01-0 | O-1 (new clause) | violation fail: O-4.model | model-call marks 2, stub requests after first use 0 | O-4.model | unresolved (not exercised) |
| O-1-model-not-durable | A15-1 | O-1 (new clause) | violation fail: O-4.model | model-call marks 1, stub requests after first use 0 | - | unresolved (not exercised) |
| O-1-model-not-durable | A16-1 | O-1 (new clause) | violation fail: O-4.model | model-call marks 1, stub requests after first use 0 | - | unresolved (not exercised) |
| O-2-second-writer | none-S-M-0 | O-2 (new clause) | pass | second-writer marks 0, distinct replies in tool args 1 | - | unresolved (not exercised) |
| O-3-ti-no-key | none-S-M-0 | O-3 (new clause) | pass | ti-no-key marks 1, T-I effect lines 1 | - | unresolved (not exercised) |
| O-3-ti-no-key | A12-0 | O-3 (new clause) | pass | ti-no-key marks 1, T-I effect lines 1 | - | unresolved (not exercised) |
| O-3-ti-no-key | A15-1 | O-3 (new clause) | pass | ti-no-key marks 1, T-I effect lines 1 | - | unresolved (not exercised) |
| O-4-attempt-not-recorded | none-S-M-0 | O-4 (new clause) | pass | attempt-skipped marks 0, T-N requests 1 | - | unresolved (not exercised) |
| O-4-attempt-not-recorded | A13-0 | O-4 (new clause) | pass | attempt-skipped marks 0, T-N requests 1 | - | unresolved (not exercised) |
| O-4-attempt-not-recorded | A17-1 | O-4 (new clause) | pass | attempt-skipped marks 0, T-N requests 1 | - | unresolved (not exercised) |
| O-5-event-dropped-hang | A09-0 | O-5 (new clause) | unresolved unresolved: O-1.end_state, O-2.memory, O-4.approval, O-4.calls, O-4.model, O-4.model_used, O-4.policy, O-4.retention, O-5.once, O-5.proceeds | notifications deleted at worker start: 1 | - | unresolved (cap) |
| O-5-recv-no-wait | A09-0 | O-5 (new clause) | pass | recv(timeout 0) returned ['approve'] | - | unresolved (not exercised) |
| O-5-recv-no-wait | A09-1 | O-5 (new clause) | pass | recv(timeout 0) returned ['deny'] | - | unresolved (not exercised) |
| O-5-recv-no-wait | A10-0 | O-5 (new clause) | pass | recv(timeout 0) returned ['approve'] | - | unresolved (not exercised) |
| O-5-recv-no-wait | A10-1 | O-5 (new clause) | pass | recv(timeout 0) returned ['deny'] | - | unresolved (not exercised) |
| O-4-rule-version-not-recorded | A02-0 | O-4 (new clause) | violation fail: O-4.calls, O-4.policy | no-rule-version marks 1 | O-4.calls | no-credit (step-1 failure) |
| O-4-rule-version-not-recorded | A02-1 | O-4 (new clause) | violation fail: O-4.calls, O-4.policy | no-rule-version marks 1 | O-4.calls | no-credit (step-1 failure) |
| O-1-model-not-durable | A02-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model | model-call marks 2, stub requests after first use 1 | O-4.calls | flagged |
| O-1-model-not-durable | A02-1 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model | model-call marks 2, stub requests after first use 1 | O-4.calls | flagged |
| O-1-model-not-durable | A03-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model | model-call marks 2, stub requests after first use 1 | O-4.calls | flagged |
| O-1-model-not-durable | A04-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-2.memory, O-4.model | model-call marks 2, stub requests after first use 1 | - | flagged |
| O-1-model-not-durable | A04-1 | O-1 (new clause) | violation fail: O-1.no_recall, O-2.memory, O-4.model | model-call marks 2, stub requests after first use 1 | - | flagged |
| O-1-model-not-durable | A06-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model | model-call marks 2, stub requests after first use 1 | O-4.calls | flagged |
| O-1-model-not-durable | A06-1 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model | model-call marks 2, stub requests after first use 1 | - | flagged |
| O-1-model-not-durable | A07-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model | model-call marks 2, stub requests after first use 1 | O-4.calls | flagged |
| O-1-model-not-durable | A11-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model | model-call marks 3, stub requests after first use 2 | O-4.calls | flagged |
| O-1-model-not-durable | A15-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model | model-call marks 2, stub requests after first use 1 | O-4.calls | flagged |
| O-1-model-not-durable | A16-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model | model-call marks 2, stub requests after first use 1 | O-4.calls | flagged |
| O-2-second-writer | A01-0 | O-2 (new clause) | invalid fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.tf_stale, O-4.calls, O-4.model invalid: ["T-F: delivered line without its request ('T:T-F', 'T:T-F', 1)"] | second-writer marks 1, distinct replies in tool args 2 | O-4.model | flagged |
| O-2-second-writer | A02-0 | O-2 (new clause) | invalid fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.tf_stale, O-4.calls, O-4.model invalid: ["T-F: delivered line without its request ('T:T-F', 'T:T-F', 1)"] | second-writer marks 1, distinct replies in tool args 2 | O-4.calls | flagged |
| O-2-second-writer | A02-1 | O-2 (new clause) | invalid fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.tf_stale, O-4.calls, O-4.model invalid: ["T-F: delivered line without its request ('T:T-F', 'T:T-F', 1)"] | second-writer marks 1, distinct replies in tool args 2 | O-4.calls | flagged |
| O-2-second-writer | A05-0 | O-2 (new clause) | invalid fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.tf_stale, O-4.calls, O-4.model invalid: ["T-F: delivered line without its request ('T:T-F', 'T:T-F', 1)"] | second-writer marks 1, distinct replies in tool args 2 | O-4.model | flagged |
| O-2-second-writer | A07-0 | O-2 (new clause) | invalid fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.tf_stale, O-4.calls, O-4.model invalid: ["T-F: delivered line without its request ('T:T-F', 'T:T-F', 1)"] | second-writer marks 1, distinct replies in tool args 2 | O-4.calls | flagged |
| O-2-second-writer | A14-0 | O-2 (new clause) | invalid fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.tf_stale, O-4.calls, O-4.model invalid: ["T-F: delivered line without its request ('T:T-F', 'T:T-F', 1)"] | second-writer marks 1, distinct replies in tool args 2 | O-4.model | flagged |
| O-2-second-writer | A14-1 | O-2 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.tn_no_repeat, O-4.calls, O-4.model | second-writer marks 1, distinct replies in tool args 2 | - | flagged |
| O-2-second-writer | A15-0 | O-2 (new clause) | invalid fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.tf_stale, O-4.calls, O-4.model invalid: ["T-F: delivered line without its request ('T:T-F', 'T:T-F', 1)"] | second-writer marks 1, distinct replies in tool args 2 | O-4.calls | flagged |
| O-2-second-writer | A15-1 | O-2 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.tn_no_repeat, O-4.calls, O-4.model | second-writer marks 1, distinct replies in tool args 2 | - | flagged |
| O-2-second-writer | A17-0 | O-2 (new clause) | invalid fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.tf_stale, O-4.calls, O-4.model invalid: ["T-F: delivered line without its request ('T:T-F', 'T:T-F', 1)"] | second-writer marks 1, distinct replies in tool args 2 | - | flagged |
| O-2-second-writer | A17-1 | O-2 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.tn_no_repeat, O-4.calls, O-4.model | second-writer marks 1, distinct replies in tool args 2 | - | flagged |
| O-3-ti-no-key | A02-0 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once, O-4.calls | ti-no-key marks 2, T-I effect lines 2 | O-4.calls | flagged |
| O-3-ti-no-key | A02-1 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once, O-4.calls | ti-no-key marks 2, T-I effect lines 2 | O-4.calls | flagged |
| O-3-ti-no-key | A06-0 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once, O-4.calls | ti-no-key marks 2, T-I effect lines 2 | O-4.calls | flagged |
| O-3-ti-no-key | A06-1 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once, O-4.calls | ti-no-key marks 2, T-I effect lines 2 | - | flagged |
| O-3-ti-no-key | A11-0 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once, O-4.calls | ti-no-key marks 3, T-I effect lines 3 | O-4.calls | flagged |
| O-3-ti-no-key | A15-0 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once, O-4.calls | ti-no-key marks 2, T-I effect lines 2 | O-4.calls | flagged |
| O-4-attempt-not-recorded | A04-0 | O-4 (new clause) | violation fail: O-4.calls | attempt-skipped marks 1, T-N requests 1 | - | flagged |
| O-4-attempt-not-recorded | A04-1 | O-4 (new clause) | violation fail: O-4.calls | attempt-skipped marks 1, T-N requests 1 | - | flagged |
| O-4-attempt-not-recorded | A08-0 | O-4 (new clause) | violation fail: O-4.calls | attempt-skipped marks 1, T-N requests 1 | - | flagged |
| O-4-attempt-not-recorded | A08-1 | O-4 (new clause) | violation fail: O-4.calls | attempt-skipped marks 1, T-N requests 1 | - | flagged |
| O-4-attempt-not-recorded | A17-0 | O-4 (new clause) | violation fail: O-4.calls | attempt-skipped marks 1, T-N requests 2 | - | flagged |
| O-4-rule-version-not-recorded | none-S-M-0 | O-4 (new clause) | violation fail: O-4.policy | no-rule-version marks 1 | - | flagged |
| O-4-rule-version-not-recorded | A09-0 | O-4 (new clause) | violation fail: O-4.policy | no-rule-version marks 1 | - | flagged |
| O-4-rule-version-not-recorded | A09-1 | O-4 (new clause) | violation fail: O-4.policy | no-rule-version marks 1 | - | flagged |
| O-4-rule-version-not-recorded | A10-0 | O-4 (new clause) | violation fail: O-4.policy | no-rule-version marks 1 | - | flagged |
| O-4-rule-version-not-recorded | A10-1 | O-4 (new clause) | violation fail: O-4.policy | no-rule-version marks 1 | - | flagged |
| O-5-event-dropped | A09-0 | O-5 (new clause) | violation fail: O-1.end_state, O-2.memory, O-4.approval, O-5.once, O-5.proceeds | notifications deleted at worker start: 1 | - | flagged |
| O-5-event-dropped | A09-1 | O-5 (new clause) | violation fail: O-4.approval, O-5.once | notifications deleted at worker start: 1 | - | flagged |
| O-5-event-dropped | A10-0 | O-5 (new clause) | violation fail: O-1.end_state, O-2.memory, O-4.approval, O-5.once, O-5.proceeds | notifications deleted at worker start: 1 | - | flagged |
| O-5-event-dropped | A10-1 | O-5 (new clause) | violation fail: O-4.approval, O-5.once | notifications deleted at worker start: 1 | - | flagged |
| O-5-wait-skipped | A09-0 | O-5 (new clause) | violation fail: O-4.approval, O-5.once | wait-skipped marks 1, tool requests 3 (0 before posted) | - | flagged |
| O-5-wait-skipped | A09-1 | O-5 (new clause) | violation fail: O-1.end_state, O-2.memory, O-2.single_exec, O-3.deny_no_tool, O-4.approval, O-5.once, O-5.proceeds | wait-skipped marks 1, tool requests 3 (0 before posted) | - | flagged |
| O-5-wait-skipped | A10-0 | O-5 (new clause) | violation fail: O-4.approval, O-5.once | wait-skipped marks 2, tool requests 3 (0 before posted) | - | flagged |
| O-5-wait-skipped | A10-1 | O-5 (new clause) | violation fail: O-1.end_state, O-2.memory, O-2.single_exec, O-3.deny_no_tool, O-4.approval, O-5.once, O-5.proceeds | wait-skipped marks 2, tool requests 3 (0 before posted) | - | flagged |

| L2 fault (restate) | run | expected | checker result | proxy evidence | status |
|---|---|---|---|---|---|
| attempt_drop | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| attempt_drop | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| attempt_outcome | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| attempt_outcome | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| call_args | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| call_args | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| call_drop | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| call_drop | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| call_token | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| call_token | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| outcome_result | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| outcome_result | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| approval_flip | A09-0/S-AP/ | O-5.once, O-4.approval | violation fail: O-4.approval, O-5.once | replayed answer | flagged |
| approval_flip | A09-1/S-AP/ | O-5.once, O-4.approval | violation fail: O-4.approval, O-5.once | replayed answer | flagged |
| approval_flip | A10-0/S-AP/ | O-5.once, O-4.approval | violation fail: O-4.approval, O-5.once | replayed answer | flagged |
| approval_null | A09-0/S-AP/ | O-5.once, O-4.approval | violation fail: O-4.approval, O-5.once | replayed answer | flagged |
| approval_null | A09-1/S-AP/ | O-5.once, O-4.approval | violation fail: O-4.approval, O-5.once | replayed answer | flagged |
| approval_null | A10-0/S-AP/ | O-5.once, O-4.approval | violation fail: O-4.approval, O-5.once | replayed answer | flagged |
| approval_without_event | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| approval_without_event | A02-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| approval_without_event | A04-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| approval_without_event | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| approval_without_event | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| attempt_drop | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_drop | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_drop | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_outcome | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_outcome | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_outcome | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_args | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_args | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_args | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_drop | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_drop | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_drop | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_token | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_token | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_token | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| deliver_drop_fail | A09/S-AP/1 | O-5.accepted | violation fail: O-5.accepted unresolved: O-1.end_state, O-2.memory, O-4.approval, O-4.calls, O-4.model, O-4.model_used, O-4.policy, O-4.retention, O-5.once, O-5.proceeds | proxy applied 1x, errors 0 | flagged (capped run) |
| deliver_flip | A09/S-AP/0 | O-5.proceeds | violation fail: O-1.end_state, O-2.memory, O-4.approval, O-5.once, O-5.proceeds | proxy applied 1x, errors 0 | flagged |
| deliver_flip | A09/S-AP/1 | O-5.proceeds | violation fail: O-1.end_state, O-2.memory, O-2.single_exec, O-3.deny_no_tool, O-4.approval, O-5.once, O-5.proceeds | proxy applied 1x, errors 0 | flagged |
| mem_drop_tn | none-S-M-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_drop_tn | A02-0/S-M/ | O-2.memory | violation fail: O-2.memory, O-4.calls | replayed answer | flagged |
| mem_drop_tn | A04-0/S-M/ | O-2.memory | violation fail: O-2.memory, O-4.calls | replayed answer | flagged |
| mem_drop_tn | A06-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_drop_tn | A15-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_extra_key | none-S-M-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_extra_key | A02-0/S-M/ | O-2.memory | violation fail: O-2.memory, O-4.calls | replayed answer | flagged |
| mem_extra_key | A04-0/S-M/ | O-2.memory | violation fail: O-2.memory, O-4.calls | replayed answer | flagged |
| mem_extra_key | A06-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_extra_key | A15-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_reply | none-S-M-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_reply | A02-0/S-M/ | O-2.memory | violation fail: O-2.memory, O-4.calls | replayed answer | flagged |
| mem_reply | A04-0/S-M/ | O-2.memory | violation fail: O-2.memory, O-4.calls | replayed answer | flagged |
| mem_reply | A06-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_reply | A15-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_tf | none-S-M-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_tf | A02-0/S-M/ | O-2.memory | violation fail: O-2.memory, O-4.calls | replayed answer | flagged |
| mem_tf | A04-0/S-M/ | O-2.memory | violation fail: O-2.memory, O-4.calls | replayed answer | flagged |
| mem_tf | A06-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_tf | A15-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| model_drop | none-S-M-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_drop | A02-0/S-M/ | O-4.model | violation fail: O-4.calls, O-4.model | replayed answer | flagged |
| model_drop | A04-0/S-M/ | O-4.model | violation fail: O-4.calls, O-4.model | replayed answer | flagged |
| model_drop | A06-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_drop | A15-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_extra | none-S-M-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_extra | A02-0/S-M/ | O-4.model | violation fail: O-4.calls, O-4.model | replayed answer | flagged |
| model_extra | A04-0/S-M/ | O-4.model | violation fail: O-4.calls, O-4.model | replayed answer | flagged |
| model_extra | A06-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_extra | A15-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_input | none-S-M-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_input | A02-0/S-M/ | O-4.model | violation fail: O-4.calls, O-4.model | replayed answer | flagged |
| model_input | A04-0/S-M/ | O-4.model | violation fail: O-4.calls, O-4.model | replayed answer | flagged |
| model_input | A06-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_input | A15-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_output_sha | none-S-M-0/S-M/ | O-4.model | violation fail: O-4.model, O-4.model_used | replayed answer | flagged |
| model_output_sha | A02-0/S-M/ | O-4.model | violation fail: O-4.calls, O-4.model, O-4.model_used | replayed answer | flagged |
| model_output_sha | A04-0/S-M/ | O-4.model | violation fail: O-4.calls, O-4.model, O-4.model_used | replayed answer | flagged |
| model_output_sha | A06-1/S-M/ | O-4.model | violation fail: O-4.model, O-4.model_used | replayed answer | flagged |
| model_output_sha | A15-1/S-M/ | O-4.model | violation fail: O-4.model, O-4.model_used | replayed answer | flagged |
| model_used_false | none-S-M-0/S-M/ | O-4.model_used | violation fail: O-4.model_used | replayed answer | flagged |
| model_used_false | A02-0/S-M/ | O-4.model_used | violation fail: O-4.calls, O-4.model_used | replayed answer | flagged |
| model_used_false | A04-0/S-M/ | O-4.model_used | violation fail: O-4.calls, O-4.model_used | replayed answer | flagged |
| model_used_false | A06-1/S-M/ | O-4.model_used | violation fail: O-4.model_used | replayed answer | flagged |
| model_used_false | A15-1/S-M/ | O-4.model_used | violation fail: O-4.model_used | replayed answer | flagged |
| outcome_result | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| outcome_result | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| outcome_result | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| policy_action | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_action | A02-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| policy_action | A04-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| policy_action | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_action | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_drop | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_drop | A02-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| policy_drop | A04-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| policy_drop | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_drop | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_dup | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_dup | A02-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| policy_dup | A04-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| policy_dup | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_dup | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_dup | A09-0/S-AP/ | O-5.once | violation fail: O-4.approval, O-4.policy, O-5.once | replayed answer | flagged |
| policy_dup | A09-1/S-AP/ | O-5.once | violation fail: O-4.approval, O-4.policy, O-5.once | replayed answer | flagged |
| policy_dup | A10-0/S-AP/ | O-5.once | violation fail: O-4.approval, O-4.policy, O-5.once | replayed answer | flagged |
| policy_result | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_result | A02-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| policy_result | A04-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| policy_result | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_result | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| recon_retention | none-S-M-0/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| recon_retention | A02-0/S-M/ | O-4.retention | violation fail: O-4.calls, O-4.retention | replayed answer | flagged |
| recon_retention | A04-0/S-M/ | O-4.retention | violation fail: O-4.calls, O-4.retention | replayed answer | flagged |
| recon_retention | A06-1/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| recon_retention | A15-1/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| rule_version_null | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| rule_version_null | A02-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| rule_version_null | A04-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| rule_version_null | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| rule_version_null | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| rule_version_null | A09-0/S-AP/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| rule_version_null | A09-1/S-AP/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| rule_version_null | A10-0/S-AP/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| state_retention | none-S-M-0/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| state_retention | A02-0/S-M/ | O-4.retention | violation fail: O-4.calls, O-4.retention | replayed answer | flagged |
| state_retention | A04-0/S-M/ | O-4.retention | violation fail: O-4.calls, O-4.retention | replayed answer | flagged |
| state_retention | A06-1/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| state_retention | A15-1/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| state_task_id | none-S-M-0/S-M/ | O-1.end_state | violation fail: O-1.end_state | replayed answer | flagged |
| state_task_id | A02-0/S-M/ | O-1.end_state | violation fail: O-1.end_state, O-4.calls | replayed answer | flagged |
| state_task_id | A04-0/S-M/ | O-1.end_state | violation fail: O-1.end_state, O-4.calls | replayed answer | flagged |
| state_task_id | A06-1/S-M/ | O-1.end_state | violation fail: O-1.end_state | replayed answer | flagged |
| state_task_id | A15-1/S-M/ | O-1.end_state | violation fail: O-1.end_state | replayed answer | flagged |
| status_done_to_doubt | none-S-M-0/S-M/ | O-1.end_state | violation fail: O-1.end_state, O-2.memory | replayed answer | flagged |
| status_done_to_doubt | A02-0/S-M/ | O-1.end_state | violation fail: O-1.end_state, O-2.memory, O-4.calls | replayed answer | flagged |
| status_done_to_doubt | A04-0/S-M/ | O-2.memory | violation fail: O-2.memory, O-4.calls | replayed answer | flagged |
| status_done_to_doubt | A06-1/S-M/ | O-1.end_state | violation fail: O-1.end_state, O-2.memory | replayed answer | flagged |
| status_done_to_doubt | A15-1/S-M/ | O-1.end_state | violation fail: O-1.end_state, O-2.memory | replayed answer | flagged |
| deliver_twice | A09/S-AP/0 | none (must pass) | pass | proxy applied 1x, errors 0 | correct-pass |
| passthrough | none/S-M/0 | none (must pass) | pass | proxy applied 0x, errors 0 | correct-pass |
| passthrough | A09/S-AP/1 | none (must pass) | pass | proxy applied 0x, errors 0 | correct-pass |

| control (restate, unmodified) | checker result | harness | detail | status |
|---|---|---|---|---|
| A01-0 | pass | ended | - | pass |
| A02-0 | violation fail: O-4.calls | ended | O-4.calls: T-I 'T:T-I' attempt 1: not in reconstruct (or call fields differ) | step-1 finding |
| A02-1 | violation fail: O-4.calls | ended | O-4.calls: T-I 'T:T-I' attempt 1: not in reconstruct (or call fields differ) | step-1 finding |
| A03-0 | violation fail: O-4.calls | ended | O-4.calls: T-F 'T:T-F' attempt 1: not in reconstruct (or call fields differ) | step-1 finding |
| A04-0 | violation fail: O-4.calls | ended | O-4.calls: T-N 'T:T-N' attempt 1: not in reconstruct (or call fields differ); call 'T:T-N': outcome 'owner:happened', last attempt 'unknown' | step-1 finding |
| A04-1 | violation fail: O-4.calls | ended | O-4.calls: T-N 'T:T-N' attempt 1: not in reconstruct (or call fields differ); call 'T:T-N': outcome 'owner:happened', last attempt 'unknown' | step-1 finding |
| A05-0 | pass | ended | - | pass |
| A06-0 | pass | ended | - | pass |
| A06-1 | pass | ended | - | pass |
| A07-0 | pass | ended | - | pass |
| A08-0 | pass | ended | - | pass |
| A08-1 | violation fail: O-4.calls | ended | O-4.calls: T-N 'T:T-N' attempt 1: not in reconstruct (or call fields differ); call 'T:T-N': outcome 'owner:happened', last attempt 'unknown' | step-1 finding |
| A09-0 | pass | ended | - | pass |
| A09-1 | pass | ended | - | pass |
| A10-0 | pass | ended | - | pass |
| A10-1 | pass | ended | - | pass |
| A11-0 | violation fail: O-4.calls | ended | O-4.calls: T-I 'T:T-I' attempt 1: not in reconstruct (or call fields differ); T-I 'T:T-I' attempt 2: not in reconstruct (or call fields differ) | step-1 finding |
| A12-0 | violation fail: O-4.calls | ended | O-4.calls: T-I 'T:T-I' attempt 1: not in reconstruct (or call fields differ) | step-1 finding |
| A13-0 | violation fail: O-4.calls | ended | O-4.calls: T-N 'T:T-N' attempt 1: not in reconstruct (or call fields differ) | step-1 finding |
| A14-0 | pass | ended | - | pass |
| A14-1 | pass | ended | - | pass |
| A15-0 | violation fail: O-4.calls | ended | O-4.calls: T-I 'T:T-I' attempt 1: not in reconstruct (or call fields differ) | step-1 finding |
| A15-1 | pass | ended | - | pass |
| A16-0 | violation fail: O-4.calls | ended | O-4.calls: T-F 'T:T-F' attempt 1: not in reconstruct (or call fields differ) | step-1 finding |
| A16-1 | pass | ended | - | pass |
| A17-0 | violation fail: O-4.calls | ended | O-4.calls: T-N 'T:T-N' attempt 1: not in reconstruct (or call fields differ) | step-1 finding |
| A17-1 | pass | ended | - | pass |
| none-S-M-0 | pass | ended | - | pass |

| L3 mutant (restate) | run | expected | checker result | mutation evidence | control fails | status |
|---|---|---|---|---|---|---|
| O-1-model-not-durable | none-S-M-0 | O-1 (new clause) | violation fail: O-4.calls, O-4.model | model-call marks 1, stub requests after first use 0 | - | unresolved (not exercised) |
| O-1-model-not-durable | A15-1 | O-1 (new clause) | violation fail: O-4.calls, O-4.model | model-call marks 1, stub requests after first use 0 | - | unresolved (not exercised) |
| O-2-second-writer | none-S-M-0 | O-2 (new clause) | pass | second-writer marks 0, distinct replies in tool args 1 | - | unresolved (not exercised) |
| O-3-ti-no-key | none-S-M-0 | O-3 (new clause) | pass | ti-no-key marks 0, T-I effect lines 1 | - | unresolved (not exercised) |
| O-3-ti-no-key | A06-0 | O-3 (new clause) | pass | ti-no-key marks 0, T-I effect lines 1 | - | unresolved (not exercised) |
| O-3-ti-no-key | A06-1 | O-3 (new clause) | pass | ti-no-key marks 0, T-I effect lines 1 | - | unresolved (not exercised) |
| O-3-ti-no-key | A15-1 | O-3 (new clause) | pass | ti-no-key marks 0, T-I effect lines 1 | - | unresolved (not exercised) |
| O-4-attempt-not-recorded | none-S-M-0 | O-4 (new clause) | pass | attempt-skipped marks 0, T-N requests 1 | - | unresolved (not exercised) |
| O-4-attempt-not-recorded | A15-1 | O-4 (new clause) | pass | attempt-skipped marks 0, T-N requests 1 | - | unresolved (not exercised) |
| O-5-wait-skipped | A09-0 | O-5 (new clause) | pass | wait-skipped marks 1, tool requests 3 (0 before posted), approval record present | - | unresolved (not exercised) |
| O-5-wait-skipped | A10-0 | O-5 (new clause) | pass | wait-skipped marks 1, tool requests 3 (0 before posted), approval record present | - | unresolved (not exercised) |
| O-4-attempt-not-recorded | A02-0 | O-4 (new clause) | violation fail: O-4.calls | attempt-skipped marks 1, T-N requests 1 | O-4.calls | no-credit (step-1 failure) |
| O-4-attempt-not-recorded | A02-1 | O-4 (new clause) | violation fail: O-4.calls | attempt-skipped marks 1, T-N requests 1 | O-4.calls | no-credit (step-1 failure) |
| O-4-attempt-not-recorded | A04-0 | O-4 (new clause) | violation fail: O-4.calls | attempt-skipped marks 1, T-N requests 1 | O-4.calls | no-credit (step-1 failure) |
| O-4-attempt-not-recorded | A04-1 | O-4 (new clause) | violation fail: O-4.calls | attempt-skipped marks 1, T-N requests 1 | O-4.calls | no-credit (step-1 failure) |
| O-4-attempt-not-recorded | A15-0 | O-4 (new clause) | violation fail: O-4.calls | attempt-skipped marks 1, T-N requests 1 | O-4.calls | no-credit (step-1 failure) |
| O-1-model-not-durable | A02-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model | model-call marks 2, stub requests after first use 1 | O-4.calls | flagged |
| O-1-model-not-durable | A04-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-2.memory, O-4.calls, O-4.model | model-call marks 2, stub requests after first use 1 | O-4.calls | flagged |
| O-1-model-not-durable | A04-1 | O-1 (new clause) | violation fail: O-1.no_recall, O-2.memory, O-4.calls, O-4.model | model-call marks 2, stub requests after first use 1 | O-4.calls | flagged |
| O-1-model-not-durable | A06-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model | model-call marks 2, stub requests after first use 1 | - | flagged |
| O-1-model-not-durable | A06-1 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model | model-call marks 2, stub requests after first use 1 | - | flagged |
| O-1-model-not-durable | A11-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model | model-call marks 3, stub requests after first use 2 | O-4.calls | flagged |
| O-1-model-not-durable | A15-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model | model-call marks 2, stub requests after first use 1 | O-4.calls | flagged |
| O-2-second-writer | A02-0 | O-2 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.tn_no_repeat, O-4.calls, O-4.model | second-writer marks 1, distinct replies in tool args 2 | O-4.calls | flagged |
| O-2-second-writer | A05-0 | O-2 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.tn_no_repeat, O-4.calls, O-4.model | second-writer marks 1, distinct replies in tool args 2 | - | flagged |
| O-2-second-writer | A15-0 | O-2 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.tn_no_repeat, O-4.calls, O-4.model | second-writer marks 1, distinct replies in tool args 2 | O-4.calls | flagged |
| O-2-second-writer | A15-1 | O-2 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.tn_no_repeat, O-4.calls, O-4.model | second-writer marks 1, distinct replies in tool args 2 | - | flagged |
| O-3-ti-no-key | A02-0 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once, O-4.calls | ti-no-key marks 0, T-I effect lines 2 | O-4.calls | flagged |
| O-3-ti-no-key | A02-1 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once, O-4.calls | ti-no-key marks 0, T-I effect lines 2 | O-4.calls | flagged |
| O-3-ti-no-key | A11-0 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once, O-4.calls | ti-no-key marks 0, T-I effect lines 3 | O-4.calls | flagged |
| O-3-ti-no-key | A15-0 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once, O-4.calls | ti-no-key marks 0, T-I effect lines 2 | O-4.calls | flagged |
| O-4-rule-version-not-recorded | none-S-M-0 | O-4 (new clause) | violation fail: O-4.policy | no-rule-version marks 1 | - | flagged |
| O-4-rule-version-not-recorded | A09-0 | O-4 (new clause) | violation fail: O-4.policy | no-rule-version marks 1 | - | flagged |
| O-4-rule-version-not-recorded | A09-1 | O-4 (new clause) | violation fail: O-4.policy | no-rule-version marks 1 | - | flagged |
| O-4-rule-version-not-recorded | A10-0 | O-4 (new clause) | violation fail: O-4.policy | no-rule-version marks 1 | - | flagged |
| O-4-rule-version-not-recorded | A10-1 | O-4 (new clause) | violation fail: O-4.policy | no-rule-version marks 1 | - | flagged |
| O-5-wait-skipped | A09-1 | O-5 (new clause) | violation fail: O-1.end_state, O-2.memory, O-2.single_exec, O-3.deny_no_tool, O-4.approval, O-5.proceeds | wait-skipped marks 1, tool requests 3 (0 before posted), approval record present | - | flagged |
| O-5-wait-skipped | A10-1 | O-5 (new clause) | violation fail: O-1.end_state, O-2.memory, O-2.single_exec, O-3.deny_no_tool, O-4.approval, O-5.proceeds | wait-skipped marks 1, tool requests 3 (0 before posted), approval record present | - | flagged |

| L2 fault (temporal) | run | expected | checker result | proxy evidence | status |
|---|---|---|---|---|---|
| attempt_drop | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| attempt_drop | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| attempt_outcome | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| attempt_outcome | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| call_args | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| call_args | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| call_drop | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| call_drop | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| call_token | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| call_token | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| outcome_result | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | no-credit (control fails this clause) |
| approval_flip | A09-0/S-AP/ | O-5.once, O-4.approval | violation fail: O-4.approval, O-5.once | replayed answer | flagged |
| approval_flip | A09-1/S-AP/ | O-5.once, O-4.approval | violation fail: O-4.approval, O-5.once | replayed answer | flagged |
| approval_flip | A10-0/S-AP/ | O-5.once, O-4.approval | violation fail: O-4.approval, O-4.calls, O-5.once | replayed answer | flagged |
| approval_null | A09-0/S-AP/ | O-5.once, O-4.approval | violation fail: O-4.approval, O-5.once | replayed answer | flagged |
| approval_null | A09-1/S-AP/ | O-5.once, O-4.approval | violation fail: O-4.approval, O-5.once | replayed answer | flagged |
| approval_null | A10-0/S-AP/ | O-5.once, O-4.approval | violation fail: O-4.approval, O-4.calls, O-5.once | replayed answer | flagged |
| approval_without_event | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| approval_without_event | A02-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| approval_without_event | A04-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| approval_without_event | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| approval_without_event | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| attempt_drop | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_drop | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_drop | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_outcome | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_outcome | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_outcome | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_args | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_args | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_args | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_drop | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_drop | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_drop | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_token | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_token | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_token | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| deliver_drop_fail | A09/S-AP/1 | O-5.accepted | violation fail: O-5.accepted unresolved: O-1.end_state, O-2.memory, O-4.approval, O-4.calls, O-4.model, O-4.model_used, O-4.policy, O-4.retention, O-5.once, O-5.proceeds | proxy applied 1x, errors 0 | flagged (capped run) |
| deliver_flip | A09/S-AP/0 | O-5.proceeds | violation fail: O-1.end_state, O-2.memory, O-4.approval, O-5.once, O-5.proceeds | proxy applied 1x, errors 0 | flagged |
| deliver_flip | A09/S-AP/1 | O-5.proceeds | violation fail: O-1.end_state, O-2.memory, O-2.single_exec, O-3.deny_no_tool, O-4.approval, O-5.once, O-5.proceeds | proxy applied 1x, errors 0 | flagged |
| mem_drop_tn | none-S-M-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_drop_tn | A02-0/S-M/ | O-2.memory | violation fail: O-2.memory, O-4.calls | replayed answer | flagged |
| mem_drop_tn | A04-0/S-M/ | O-2.memory | violation fail: O-2.memory, O-4.calls | replayed answer | flagged |
| mem_drop_tn | A06-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_drop_tn | A15-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_extra_key | none-S-M-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_extra_key | A02-0/S-M/ | O-2.memory | violation fail: O-2.memory, O-4.calls | replayed answer | flagged |
| mem_extra_key | A04-0/S-M/ | O-2.memory | violation fail: O-2.memory, O-4.calls | replayed answer | flagged |
| mem_extra_key | A06-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_extra_key | A15-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_reply | none-S-M-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_reply | A02-0/S-M/ | O-2.memory | violation fail: O-2.memory, O-4.calls | replayed answer | flagged |
| mem_reply | A04-0/S-M/ | O-2.memory | violation fail: O-2.memory, O-4.calls | replayed answer | flagged |
| mem_reply | A06-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_reply | A15-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_tf | none-S-M-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_tf | A02-0/S-M/ | O-2.memory | violation fail: O-2.memory, O-4.calls | replayed answer | flagged |
| mem_tf | A04-0/S-M/ | O-2.memory | violation fail: O-2.memory, O-4.calls | replayed answer | flagged |
| mem_tf | A06-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_tf | A15-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| model_drop | none-S-M-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_drop | A02-0/S-M/ | O-4.model | violation fail: O-4.calls, O-4.model | replayed answer | flagged |
| model_drop | A04-0/S-M/ | O-4.model | violation fail: O-4.calls, O-4.model | replayed answer | flagged |
| model_drop | A06-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_drop | A15-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_extra | none-S-M-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_extra | A02-0/S-M/ | O-4.model | violation fail: O-4.calls, O-4.model | replayed answer | flagged |
| model_extra | A04-0/S-M/ | O-4.model | violation fail: O-4.calls, O-4.model | replayed answer | flagged |
| model_extra | A06-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_extra | A15-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_input | none-S-M-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_input | A02-0/S-M/ | O-4.model | violation fail: O-4.calls, O-4.model | replayed answer | flagged |
| model_input | A04-0/S-M/ | O-4.model | violation fail: O-4.calls, O-4.model | replayed answer | flagged |
| model_input | A06-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_input | A15-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_output_sha | none-S-M-0/S-M/ | O-4.model | violation fail: O-4.model, O-4.model_used | replayed answer | flagged |
| model_output_sha | A02-0/S-M/ | O-4.model | violation fail: O-4.calls, O-4.model, O-4.model_used | replayed answer | flagged |
| model_output_sha | A04-0/S-M/ | O-4.model | violation fail: O-4.calls, O-4.model, O-4.model_used | replayed answer | flagged |
| model_output_sha | A06-1/S-M/ | O-4.model | violation fail: O-4.model, O-4.model_used | replayed answer | flagged |
| model_output_sha | A15-1/S-M/ | O-4.model | violation fail: O-4.model, O-4.model_used | replayed answer | flagged |
| model_used_false | none-S-M-0/S-M/ | O-4.model_used | violation fail: O-4.model_used | replayed answer | flagged |
| model_used_false | A02-0/S-M/ | O-4.model_used | violation fail: O-4.calls, O-4.model_used | replayed answer | flagged |
| model_used_false | A04-0/S-M/ | O-4.model_used | violation fail: O-4.calls, O-4.model_used | replayed answer | flagged |
| model_used_false | A06-1/S-M/ | O-4.model_used | violation fail: O-4.model_used | replayed answer | flagged |
| model_used_false | A15-1/S-M/ | O-4.model_used | violation fail: O-4.model_used | replayed answer | flagged |
| outcome_result | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| outcome_result | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| outcome_result | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| policy_action | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_action | A02-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| policy_action | A04-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| policy_action | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_action | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_drop | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_drop | A02-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| policy_drop | A04-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| policy_drop | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_drop | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_dup | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_dup | A02-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| policy_dup | A04-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| policy_dup | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_dup | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_dup | A09-0/S-AP/ | O-5.once | violation fail: O-4.approval, O-4.policy, O-5.once | replayed answer | flagged |
| policy_dup | A09-1/S-AP/ | O-5.once | violation fail: O-4.approval, O-4.policy, O-5.once | replayed answer | flagged |
| policy_dup | A10-0/S-AP/ | O-5.once | violation fail: O-4.approval, O-4.calls, O-4.policy, O-5.once | replayed answer | flagged |
| policy_result | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_result | A02-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| policy_result | A04-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| policy_result | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_result | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| recon_retention | none-S-M-0/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| recon_retention | A02-0/S-M/ | O-4.retention | violation fail: O-4.calls, O-4.retention | replayed answer | flagged |
| recon_retention | A04-0/S-M/ | O-4.retention | violation fail: O-4.calls, O-4.retention | replayed answer | flagged |
| recon_retention | A06-1/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| recon_retention | A15-1/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| rule_version_null | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| rule_version_null | A02-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| rule_version_null | A04-0/S-M/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| rule_version_null | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| rule_version_null | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| rule_version_null | A09-0/S-AP/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| rule_version_null | A09-1/S-AP/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| rule_version_null | A10-0/S-AP/ | O-4.policy | violation fail: O-4.calls, O-4.policy | replayed answer | flagged |
| state_retention | none-S-M-0/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| state_retention | A02-0/S-M/ | O-4.retention | violation fail: O-4.calls, O-4.retention | replayed answer | flagged |
| state_retention | A04-0/S-M/ | O-4.retention | violation fail: O-4.calls, O-4.retention | replayed answer | flagged |
| state_retention | A06-1/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| state_retention | A15-1/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| state_task_id | none-S-M-0/S-M/ | O-1.end_state | violation fail: O-1.end_state | replayed answer | flagged |
| state_task_id | A02-0/S-M/ | O-1.end_state | violation fail: O-1.end_state, O-4.calls | replayed answer | flagged |
| state_task_id | A04-0/S-M/ | O-1.end_state | violation fail: O-1.end_state, O-4.calls | replayed answer | flagged |
| state_task_id | A06-1/S-M/ | O-1.end_state | violation fail: O-1.end_state | replayed answer | flagged |
| state_task_id | A15-1/S-M/ | O-1.end_state | violation fail: O-1.end_state | replayed answer | flagged |
| status_done_to_doubt | none-S-M-0/S-M/ | O-1.end_state | violation fail: O-1.end_state, O-2.memory | replayed answer | flagged |
| status_done_to_doubt | A02-0/S-M/ | O-1.end_state | violation fail: O-1.end_state, O-2.memory, O-4.calls | replayed answer | flagged |
| status_done_to_doubt | A04-0/S-M/ | O-2.memory | violation fail: O-2.memory, O-4.calls | replayed answer | flagged |
| status_done_to_doubt | A06-1/S-M/ | O-1.end_state | violation fail: O-1.end_state, O-2.memory | replayed answer | flagged |
| status_done_to_doubt | A15-1/S-M/ | O-1.end_state | violation fail: O-1.end_state, O-2.memory | replayed answer | flagged |
| deliver_twice | A09/S-AP/0 | none (must pass) | pass | proxy applied 1x, errors 0 | correct-pass |
| passthrough | none/S-M/0 | none (must pass) | pass | proxy applied 0x, errors 0 | correct-pass |
| passthrough | A09/S-AP/1 | none (must pass) | pass | proxy applied 0x, errors 0 | correct-pass |

| control (temporal, unmodified) | checker result | harness | detail | status |
|---|---|---|---|---|
| A01-0 | pass | ended | - | pass |
| A02-0 | violation fail: O-4.calls | ended | O-4.calls: call 'T:T-I': outcome None, last attempt 'unknown' | step-1 finding |
| A02-1 | violation fail: O-4.calls | ended | O-4.calls: call 'T:T-I': outcome None, last attempt 'unknown' | step-1 finding |
| A03-0 | violation fail: O-4.calls | ended | O-4.calls: call 'T:T-F': outcome None, last attempt 'unknown' | step-1 finding |
| A04-0 | violation fail: O-4.calls | ended | O-4.calls: call 'T:T-N': outcome 'owner:happened', last attempt 'unknown' | step-1 finding |
| A04-1 | violation fail: O-4.calls | ended | O-4.calls: call 'T:T-N': outcome 'owner:happened', last attempt 'unknown' | step-1 finding |
| A05-0 | pass | ended | - | pass |
| A06-0 | violation fail: O-4.calls | ended | O-4.calls: call 'T:T-I': outcome None, last attempt 'unknown' | step-1 finding |
| A06-1 | pass | ended | - | pass |
| A07-0 | violation fail: O-4.calls | ended | O-4.calls: call 'T:T-F': outcome None, last attempt 'unknown' | step-1 finding |
| A08-0 | violation fail: O-4.calls | ended | O-4.calls: call 'T:T-N': outcome 'owner:happened', last attempt 'unknown' | step-1 finding |
| A08-1 | pass | ended | - | pass |
| A09-0 | pass | ended | - | pass |
| A09-1 | pass | ended | - | pass |
| A10-0 | violation fail: O-4.calls | ended | O-4.calls: call 'T:T-I': outcome None, last attempt 'unknown' | step-1 finding |
| A10-1 | pass | ended | - | pass |
| A11-0 | violation fail: O-4.calls | ended | O-4.calls: call 'T:T-I': outcome None, last attempt 'unknown'; call 'T:T-I': outcome None, last attempt 'unknown' | step-1 finding |
| A12-0 | pass | ended | - | pass |
| A13-0 | pass | ended | - | pass |
| A14-0 | pass | ended | - | pass |
| A14-1 | pass | ended | - | pass |
| A15-0 | violation fail: O-4.calls | ended | O-4.calls: call 'T:T-I': outcome None, last attempt 'unknown' | step-1 finding |
| A15-1 | pass | ended | - | pass |
| A16-0 | violation fail: O-4.calls | ended | O-4.calls: call 'T:T-F': outcome None, last attempt 'unknown' | step-1 finding |
| A16-1 | pass | ended | - | pass |
| A17-0 | violation fail: O-4.calls | ended | O-4.calls: call 'T:T-N': outcome None, last attempt 'unknown' | step-1 finding |
| A17-1 | pass | ended | - | pass |
| none-S-M-0 | pass | ended | - | pass |

| L3 mutant (temporal) | run | expected | checker result | mutation evidence | control fails | status |
|---|---|---|---|---|---|---|
| O-1-model-not-durable | none-S-M-0 | O-1 (new clause) | violation fail: O-4.model | model-call marks 1, stub requests after first use 0 | - | unresolved (not exercised) |
| O-1-model-not-durable | A15-1 | O-1 (new clause) | violation fail: O-4.model | model-call marks 1, stub requests after first use 0 | - | unresolved (not exercised) |
| O-2-second-writer | none-S-M-0 | O-2 (new clause) | pass | second-writer marks 0, distinct replies in tool args 1 | - | unresolved (not exercised) |
| O-3-ti-no-key | none-S-M-0 | O-3 (new clause) | pass | ti-no-key marks 0, T-I effect lines 1 | - | unresolved (not exercised) |
| O-3-ti-no-key | A06-1 | O-3 (new clause) | pass | ti-no-key marks 0, T-I effect lines 1 | - | unresolved (not exercised) |
| O-3-ti-no-key | A15-1 | O-3 (new clause) | pass | ti-no-key marks 0, T-I effect lines 1 | - | unresolved (not exercised) |
| O-4-attempt-not-recorded | none-S-M-0 | O-4 (new clause) | pass | attempt-skipped marks 0, T-N requests 1 | - | unresolved (not exercised) |
| O-4-attempt-not-recorded | A15-1 | O-4 (new clause) | pass | attempt-skipped marks 0, T-N requests 1 | - | unresolved (not exercised) |
| O-5-event-dropped | A09-0 | O-5 (new clause) | pass | events dropped: 0 | - | unresolved (not exercised) |
| O-5-event-dropped | A09-1 | O-5 (new clause) | pass | events dropped: 0 (deny seed: timeout outcome coincides with the decision) | - | unresolved (not exercised) |
| O-5-event-dropped | A10-0 | O-5 (new clause) | violation fail: O-4.calls | events dropped: 0 | O-4.calls | unresolved (not exercised) |
| O-5-event-dropped | A10-1 | O-5 (new clause) | pass | events dropped: 0 (deny seed: timeout outcome coincides with the decision) | - | unresolved (not exercised) |
| O-5-wait-skipped | A09-0 | O-5 (new clause) | pass | wait-skipped marks 2, tool requests 3 (0 before posted), approval record present | - | unresolved (not exercised) |
| O-5-wait-skipped | A10-0 | O-5 (new clause) | violation fail: O-4.calls | wait-skipped marks 2, tool requests 3 (0 before posted), approval record present | O-4.calls | unresolved (not exercised) |
| O-4-attempt-not-recorded | A02-0 | O-4 (new clause) | violation fail: O-4.calls | attempt-skipped marks 1, T-N requests 1 | O-4.calls | no-credit (step-1 failure) |
| O-4-attempt-not-recorded | A02-1 | O-4 (new clause) | violation fail: O-4.calls | attempt-skipped marks 1, T-N requests 1 | O-4.calls | no-credit (step-1 failure) |
| O-4-attempt-not-recorded | A04-0 | O-4 (new clause) | violation fail: O-4.calls | attempt-skipped marks 1, T-N requests 1 | O-4.calls | no-credit (step-1 failure) |
| O-4-attempt-not-recorded | A04-1 | O-4 (new clause) | violation fail: O-4.calls | attempt-skipped marks 1, T-N requests 1 | O-4.calls | no-credit (step-1 failure) |
| O-4-attempt-not-recorded | A15-0 | O-4 (new clause) | violation fail: O-4.calls | attempt-skipped marks 1, T-N requests 1 | O-4.calls | no-credit (step-1 failure) |
| O-4-rule-version-not-recorded | A10-0 | O-4 (new clause) | violation fail: O-4.calls, O-4.policy | no-rule-version marks 1 | O-4.calls | no-credit (step-1 failure) |
| O-1-model-not-durable | A02-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model | model-call marks 2, stub requests after first use 1 | O-4.calls | flagged |
| O-1-model-not-durable | A04-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-2.memory, O-4.calls, O-4.model | model-call marks 2, stub requests after first use 1 | O-4.calls | flagged |
| O-1-model-not-durable | A04-1 | O-1 (new clause) | violation fail: O-1.no_recall, O-2.memory, O-4.calls, O-4.model | model-call marks 2, stub requests after first use 1 | O-4.calls | flagged |
| O-1-model-not-durable | A06-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model | model-call marks 2, stub requests after first use 1 | O-4.calls | flagged |
| O-1-model-not-durable | A06-1 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.model | model-call marks 2, stub requests after first use 1 | - | flagged |
| O-1-model-not-durable | A11-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model | model-call marks 2, stub requests after first use 2 | O-4.calls | flagged |
| O-1-model-not-durable | A15-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model | model-call marks 3, stub requests after first use 2 | O-4.calls | flagged |
| O-2-second-writer | A02-0 | O-2 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.ti_once, O-3.tn_no_repeat, O-4.calls, O-4.model | second-writer marks 1, distinct replies in tool args 2 | O-4.calls | flagged |
| O-2-second-writer | A05-0 | O-2 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.ti_once, O-3.tn_no_repeat, O-4.calls, O-4.model | second-writer marks 1, distinct replies in tool args 2 | - | flagged |
| O-2-second-writer | A15-0 | O-2 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.ti_once, O-3.tn_no_repeat, O-4.calls, O-4.model | second-writer marks 1, distinct replies in tool args 2 | O-4.calls | flagged |
| O-2-second-writer | A15-1 | O-2 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.ti_once, O-3.tn_no_repeat, O-4.calls, O-4.model | second-writer marks 1, distinct replies in tool args 2 | - | flagged |
| O-3-ti-no-key | A02-0 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once, O-4.calls | ti-no-key marks 0, T-I effect lines 2 | O-4.calls | flagged |
| O-3-ti-no-key | A02-1 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once, O-4.calls | ti-no-key marks 0, T-I effect lines 2 | O-4.calls | flagged |
| O-3-ti-no-key | A06-0 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once, O-4.calls | ti-no-key marks 0, T-I effect lines 2 | O-4.calls | flagged |
| O-3-ti-no-key | A11-0 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once, O-4.calls | ti-no-key marks 0, T-I effect lines 3 | O-4.calls | flagged |
| O-3-ti-no-key | A15-0 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once, O-4.calls | ti-no-key marks 0, T-I effect lines 2 | O-4.calls | flagged |
| O-4-rule-version-not-recorded | none-S-M-0 | O-4 (new clause) | violation fail: O-4.policy | no-rule-version marks 1 | - | flagged |
| O-4-rule-version-not-recorded | A09-0 | O-4 (new clause) | violation fail: O-4.policy | no-rule-version marks 1 | - | flagged |
| O-4-rule-version-not-recorded | A09-1 | O-4 (new clause) | violation fail: O-4.policy | no-rule-version marks 1 | - | flagged |
| O-4-rule-version-not-recorded | A10-1 | O-4 (new clause) | violation fail: O-4.policy | no-rule-version marks 1 | - | flagged |
| O-5-wait-skipped | A09-1 | O-5 (new clause) | violation fail: O-1.end_state, O-2.memory, O-2.single_exec, O-3.deny_no_tool, O-4.approval, O-5.proceeds, O-5.waits | wait-skipped marks 2, tool requests 3 (1 before posted), approval record present | - | flagged |
| O-5-wait-skipped | A10-1 | O-5 (new clause) | violation fail: O-1.end_state, O-2.memory, O-2.single_exec, O-3.deny_no_tool, O-4.approval, O-4.calls, O-5.proceeds, O-5.waits | wait-skipped marks 2, tool requests 3 (1 before posted), approval record present | - | flagged |

| L2 fault (langgraph) | run | expected | checker result | proxy evidence | status |
|---|---|---|---|---|---|
| approval_flip | A09-0/S-AP/ | O-5.once, O-4.approval | violation fail: O-4.approval, O-5.once, O-5.waits | replayed answer | flagged |
| approval_flip | A09-1/S-AP/ | O-5.once, O-4.approval | violation fail: O-4.approval, O-5.once | replayed answer | flagged |
| approval_flip | A10-0/S-AP/ | O-5.once, O-4.approval | violation fail: O-4.approval, O-5.once, O-5.waits | replayed answer | flagged |
| approval_null | A09-0/S-AP/ | O-5.once, O-4.approval | violation fail: O-4.approval, O-5.once, O-5.waits | replayed answer | flagged |
| approval_null | A09-1/S-AP/ | O-5.once, O-4.approval | violation fail: O-4.approval, O-5.once | replayed answer | flagged |
| approval_null | A10-0/S-AP/ | O-5.once, O-4.approval | violation fail: O-4.approval, O-5.once, O-5.waits | replayed answer | flagged |
| approval_without_event | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| approval_without_event | A02-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| approval_without_event | A04-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| approval_without_event | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| approval_without_event | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| attempt_drop | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_drop | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_drop | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_drop | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_drop | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_outcome | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_outcome | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_outcome | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_outcome | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_outcome | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_args | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_args | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_args | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_args | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_args | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_drop | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_drop | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_drop | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_drop | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_drop | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_token | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_token | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_token | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_token | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_token | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| deliver_drop_fail | A09/S-AP/1 | O-5.accepted | violation fail: O-5.accepted unresolved: O-1.end_state, O-2.memory, O-4.approval, O-4.calls, O-4.model, O-4.model_used, O-4.policy, O-4.retention, O-5.once, O-5.proceeds | proxy applied 1x, errors 0 | flagged (capped run) |
| deliver_flip | A09/S-AP/0 | O-5.proceeds | violation fail: O-1.end_state, O-2.memory, O-4.approval, O-5.once, O-5.proceeds | proxy applied 1x, errors 0 | flagged |
| deliver_flip | A09/S-AP/1 | O-5.proceeds | violation fail: O-1.end_state, O-2.memory, O-2.single_exec, O-3.deny_no_tool, O-4.approval, O-5.once, O-5.proceeds, O-5.waits | proxy applied 1x, errors 0 | flagged |
| deliver_twice | A09/S-AP/0 | none (must pass) | violation fail: O-5.waits | proxy applied 1x, errors 0 | flagged-extra |
| mem_drop_tn | none-S-M-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_drop_tn | A02-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_drop_tn | A04-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_drop_tn | A06-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_drop_tn | A15-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_extra_key | none-S-M-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_extra_key | A02-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_extra_key | A04-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_extra_key | A06-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_extra_key | A15-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_reply | none-S-M-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_reply | A02-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_reply | A04-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_reply | A06-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_reply | A15-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_tf | none-S-M-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_tf | A02-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_tf | A04-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_tf | A06-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| mem_tf | A15-1/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| model_drop | none-S-M-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_drop | A02-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_drop | A04-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_drop | A06-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_drop | A15-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_extra | none-S-M-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_extra | A02-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_extra | A04-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_extra | A06-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_extra | A15-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_input | none-S-M-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_input | A02-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_input | A04-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_input | A06-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_input | A15-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_output_sha | none-S-M-0/S-M/ | O-4.model | violation fail: O-4.model, O-4.model_used | replayed answer | flagged |
| model_output_sha | A02-0/S-M/ | O-4.model | violation fail: O-4.model, O-4.model_used | replayed answer | flagged |
| model_output_sha | A04-0/S-M/ | O-4.model | violation fail: O-4.model, O-4.model_used | replayed answer | flagged |
| model_output_sha | A06-1/S-M/ | O-4.model | violation fail: O-4.model, O-4.model_used | replayed answer | flagged |
| model_output_sha | A15-1/S-M/ | O-4.model | violation fail: O-4.model, O-4.model_used | replayed answer | flagged |
| model_used_false | none-S-M-0/S-M/ | O-4.model_used | violation fail: O-4.model_used | replayed answer | flagged |
| model_used_false | A02-0/S-M/ | O-4.model_used | violation fail: O-4.model_used | replayed answer | flagged |
| model_used_false | A04-0/S-M/ | O-4.model_used | violation fail: O-4.model_used | replayed answer | flagged |
| model_used_false | A06-1/S-M/ | O-4.model_used | violation fail: O-4.model_used | replayed answer | flagged |
| model_used_false | A15-1/S-M/ | O-4.model_used | violation fail: O-4.model_used | replayed answer | flagged |
| outcome_result | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| outcome_result | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| outcome_result | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| outcome_result | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| outcome_result | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| policy_action | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_action | A02-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_action | A04-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_action | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_action | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_drop | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_drop | A02-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_drop | A04-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_drop | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_drop | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_dup | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_dup | A02-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_dup | A04-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_dup | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_dup | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_dup | A09-0/S-AP/ | O-5.once | violation fail: O-4.approval, O-4.policy, O-5.once, O-5.waits | replayed answer | flagged |
| policy_dup | A09-1/S-AP/ | O-5.once | violation fail: O-4.approval, O-4.policy, O-5.once | replayed answer | flagged |
| policy_dup | A10-0/S-AP/ | O-5.once | violation fail: O-4.approval, O-4.policy, O-5.once, O-5.waits | replayed answer | flagged |
| policy_result | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_result | A02-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_result | A04-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_result | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_result | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| recon_retention | none-S-M-0/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| recon_retention | A02-0/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| recon_retention | A04-0/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| recon_retention | A06-1/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| recon_retention | A15-1/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| rule_version_null | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| rule_version_null | A02-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| rule_version_null | A04-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| rule_version_null | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| rule_version_null | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| rule_version_null | A09-0/S-AP/ | O-4.policy | violation fail: O-4.policy, O-5.waits | replayed answer | flagged |
| rule_version_null | A09-1/S-AP/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| rule_version_null | A10-0/S-AP/ | O-4.policy | violation fail: O-4.policy, O-5.waits | replayed answer | flagged |
| state_retention | none-S-M-0/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| state_retention | A02-0/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| state_retention | A04-0/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| state_retention | A06-1/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| state_retention | A15-1/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| state_task_id | none-S-M-0/S-M/ | O-1.end_state | violation fail: O-1.end_state | replayed answer | flagged |
| state_task_id | A02-0/S-M/ | O-1.end_state | violation fail: O-1.end_state | replayed answer | flagged |
| state_task_id | A04-0/S-M/ | O-1.end_state | violation fail: O-1.end_state | replayed answer | flagged |
| state_task_id | A06-1/S-M/ | O-1.end_state | violation fail: O-1.end_state | replayed answer | flagged |
| state_task_id | A15-1/S-M/ | O-1.end_state | violation fail: O-1.end_state | replayed answer | flagged |
| status_done_to_doubt | none-S-M-0/S-M/ | O-1.end_state | violation fail: O-1.end_state, O-2.memory | replayed answer | flagged |
| status_done_to_doubt | A02-0/S-M/ | O-1.end_state | violation fail: O-1.end_state, O-2.memory | replayed answer | flagged |
| status_done_to_doubt | A04-0/S-M/ | O-2.memory | violation fail: O-2.memory | replayed answer | flagged |
| status_done_to_doubt | A06-1/S-M/ | O-1.end_state | violation fail: O-1.end_state, O-2.memory | replayed answer | flagged |
| status_done_to_doubt | A15-1/S-M/ | O-1.end_state | violation fail: O-1.end_state, O-2.memory | replayed answer | flagged |
| passthrough | none/S-M/0 | none (must pass) | pass | proxy applied 0x, errors 0 | correct-pass |
| passthrough | A09/S-AP/1 | none (must pass) | pass | proxy applied 0x, errors 0 | correct-pass |

| control (langgraph, unmodified) | checker result | harness | detail | status |
|---|---|---|---|---|
| A01-0 | violation fail: O-4.model | ended | O-4.model: stub request 'rid-86a8e60a104a4bdeb8fb1bf9984581c0' (invocation 'T:model:1') missing | step-1 finding |
| A02-0 | pass | ended | - | pass |
| A02-1 | pass | ended | - | pass |
| A03-0 | violation fail: O-4.calls | ended | O-4.calls: T-F 'T:T-F' attempt 1: not in reconstruct (or call fields differ) | step-1 finding |
| A04-0 | pass | ended | - | pass |
| A04-1 | pass | ended | - | pass |
| A05-0 | violation fail: O-4.model | ended | O-4.model: stub request 'rid-72d803d1221b4ded84f169ef1eedcb29' (invocation 'T:model:1') missing | step-1 finding |
| A06-0 | pass | ended | - | pass |
| A06-1 | pass | ended | - | pass |
| A07-0 | violation fail: O-4.calls | ended | O-4.calls: T-F 'T:T-F' attempt 1: not in reconstruct (or call fields differ) | step-1 finding |
| A08-0 | pass | ended | - | pass |
| A08-1 | pass | ended | - | pass |
| A09-0 | violation fail: O-5.waits | ended | O-5.waits: tool requests before the decision was posted: ['T:T-I', 'T:T-F', 'T:T-N'] | step-1 finding |
| A09-1 | pass | ended | - | pass |
| A10-0 | violation fail: O-5.waits | ended | O-5.waits: tool requests before the decision was posted: ['T:T-I', 'T:T-F', 'T:T-N'] | step-1 finding |
| A10-1 | pass | ended | - | pass |
| A11-0 | pass | ended | - | pass |
| A12-0 | pass | ended | - | pass |
| A13-0 | pass | ended | - | pass |
| A14-0 | violation fail: O-1.one_reply, O-2.memory, O-2.single_exec, O-3.tn_no_repeat, O-4.model_used | ended | O-1.one_reply: downstream requests use 2 reply ids: ['"rid-587e0084b804448e84db576b34ec5b48"', '"rid-83ec7e16a03e437ea0227bfbd37694a0"']; O-2.memory: no single reply id; O-2.single_exec: no single reply id to derive requests from; O-3.tn_no_repeat: T-N 'T:T-N'/1 sent after 'T:T-N' without owner did_not_happen; O-4.model_used: 'rid-83ec7e16a03e437ea0227bfbd37694a0': used False, logs say True | step-1 finding |
| A14-1 | pass | ended | - | pass |
| A15-0 | violation fail: O-4.calls | ended | O-4.calls: call 'T:T-I' attempt 1: not in the logs; call 'T:T-I': outcome result differs from the log | step-1 finding |
| A15-1 | pass | ended | - | pass |
| A16-0 | pass | ended | - | pass |
| A16-1 | pass | ended | - | pass |
| A17-0 | violation fail: O-2.memory, O-4.calls | ended | O-2.memory: memory {'reply': 'rid-850143f894d84b0d853e21a8672a4442', 'tf': 'T-F-1', 'ti': 'T-I-1', 'tn': 'T-N-2'}, derived {'reply': 'rid-850143f894d84b0d853e21a8672a4442', 'ti': 'T-I-1', 'tf': 'T-F-1'} with tn in ['T-N-1']; O-4.calls: call 'T:T-N' attempt 1: not in the logs; call 'T:T-N': outcome result differs from the log | step-1 finding |
| A17-1 | pass | ended | - | pass |
| none-S-M-0 | pass | ended | - | pass |

| L3 mutant (langgraph) | run | expected | checker result | mutation evidence | control fails | status |
|---|---|---|---|---|---|---|
| O-1-model-not-durable | none-S-M-0 | O-1 (new clause) | pass | model-call marks 1, stub requests after first use 0 | - | unresolved (not exercised) |
| O-2-second-writer | none-S-M-0 | O-2 (new clause) | pass | second-writer marks 0, distinct replies in tool args 1 | - | unresolved (not exercised) |
| O-3-ti-no-key | none-S-M-0 | O-3 (new clause) | pass | ti-no-key marks 0, T-I effect lines 1 | - | unresolved (not exercised) |
| O-3-ti-no-key | A15-1 | O-3 (new clause) | pass | ti-no-key marks 0, T-I effect lines 1 | - | unresolved (not exercised) |
| O-4-attempt-not-recorded | none-S-M-0 | O-4 (new clause) | pass | attempt-skipped marks 0, T-N requests 1 | - | unresolved (not exercised) |
| O-4-attempt-not-recorded | A04-0 | O-4 (new clause) | pass | attempt-skipped marks 0, T-N requests 1 | - | unresolved (not exercised) |
| O-4-attempt-not-recorded | A04-1 | O-4 (new clause) | pass | attempt-skipped marks 0, T-N requests 1 | - | unresolved (not exercised) |
| O-4-attempt-not-recorded | A15-1 | O-4 (new clause) | pass | attempt-skipped marks 0, T-N requests 1 | - | unresolved (not exercised) |
| O-5-wait-skipped | A09-1 | O-5 (new clause) | pass | wait-skipped marks 0, tool requests 0 (0 before posted), approval record present | - | unresolved (not exercised) |
| O-4-attempt-not-recorded | A15-0 | O-4 (new clause) | pass | attempt-skipped marks 1, T-N requests 1 | O-4.calls | no-credit (step-1 failure) |
| O-5-wait-skipped | A09-0 | O-5 (new clause) | violation fail: O-5.waits | wait-skipped marks 0, tool requests 3 (3 before posted), approval record present | O-5.waits | no-credit (step-1 failure) |
| O-5-wait-skipped | A10-0 | O-5 (new clause) | violation fail: O-5.waits | wait-skipped marks 1, tool requests 5 (3 before posted), approval record present | O-5.waits | no-credit (step-1 failure) |
| O-1-model-not-durable | A02-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model_used | model-call marks 2, stub requests after first use 1 | - | flagged |
| O-1-model-not-durable | A04-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model_used | model-call marks 2, stub requests after first use 1 | - | flagged |
| O-1-model-not-durable | A04-1 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model_used | model-call marks 2, stub requests after first use 1 | - | flagged |
| O-1-model-not-durable | A06-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model_used | model-call marks 2, stub requests after first use 1 | - | flagged |
| O-1-model-not-durable | A06-1 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model_used | model-call marks 2, stub requests after first use 1 | - | flagged |
| O-1-model-not-durable | A11-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model, O-4.model_used | model-call marks 3, stub requests after first use 2 | - | flagged |
| O-1-model-not-durable | A15-0 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.model_used | model-call marks 2, stub requests after first use 1 | O-4.calls | flagged |
| O-1-model-not-durable | A15-1 | O-1 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.model_used | model-call marks 2, stub requests after first use 1 | - | flagged |
| O-2-second-writer | A02-0 | O-2 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model | second-writer marks 1, distinct replies in tool args 2 | - | flagged |
| O-2-second-writer | A05-0 | O-2 (new clause) | violation fail: O-1.one_reply, O-2.memory, O-2.single_exec, O-4.calls, O-4.model | second-writer marks 1, distinct replies in tool args 2 | O-4.model | flagged |
| O-2-second-writer | A15-0 | O-2 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.tf_stale, O-3.tn_no_repeat, O-4.calls, O-4.model | second-writer marks 1, distinct replies in tool args 2 | O-4.calls | flagged |
| O-2-second-writer | A15-1 | O-2 (new clause) | violation fail: O-1.no_recall, O-1.one_reply, O-2.memory, O-2.single_exec, O-3.tn_no_repeat, O-4.calls, O-4.model | second-writer marks 1, distinct replies in tool args 2 | - | flagged |
| O-3-ti-no-key | A02-0 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once | ti-no-key marks 0, T-I effect lines 2 | - | flagged |
| O-3-ti-no-key | A02-1 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once | ti-no-key marks 0, T-I effect lines 2 | - | flagged |
| O-3-ti-no-key | A06-0 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once | ti-no-key marks 0, T-I effect lines 2 | - | flagged |
| O-3-ti-no-key | A06-1 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once | ti-no-key marks 0, T-I effect lines 2 | - | flagged |
| O-3-ti-no-key | A11-0 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once | ti-no-key marks 0, T-I effect lines 3 | - | flagged |
| O-3-ti-no-key | A15-0 | O-3 (new clause) | violation fail: O-2.memory, O-2.single_exec, O-3.ti_once, O-4.calls | ti-no-key marks 0, T-I effect lines 2 | O-4.calls | flagged |
| O-4-attempt-not-recorded | A02-0 | O-4 (new clause) | violation fail: O-4.calls | attempt-skipped marks 1, T-N requests 1 | - | flagged |
| O-4-attempt-not-recorded | A02-1 | O-4 (new clause) | violation fail: O-4.calls | attempt-skipped marks 1, T-N requests 1 | - | flagged |
| O-4-rule-version-not-recorded | none-S-M-0 | O-4 (new clause) | violation fail: O-4.policy | no-rule-version marks 1 | - | flagged |
| O-4-rule-version-not-recorded | A09-0 | O-4 (new clause) | violation fail: O-4.policy, O-5.waits | no-rule-version marks 1 | O-5.waits | flagged |
| O-4-rule-version-not-recorded | A09-1 | O-4 (new clause) | violation fail: O-4.policy | no-rule-version marks 1 | - | flagged |
| O-4-rule-version-not-recorded | A10-0 | O-4 (new clause) | violation fail: O-4.policy, O-5.waits | no-rule-version marks 1 | O-5.waits | flagged |
| O-4-rule-version-not-recorded | A10-1 | O-4 (new clause) | violation fail: O-4.policy | no-rule-version marks 1 | - | flagged |
| O-5-wait-skipped | A10-1 | O-5 (new clause) | violation fail: O-1.end_state, O-2.memory, O-2.single_exec, O-3.deny_no_tool, O-4.approval, O-5.once, O-5.proceeds | wait-skipped marks 1, tool requests 3 (0 before posted), approval record present | - | flagged |

| L2 fault (snc) | run | expected | checker result | proxy evidence | status |
|---|---|---|---|---|---|
| approval_without_event | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| approval_without_event | A02-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| approval_without_event | A04-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| approval_without_event | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| approval_without_event | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| attempt_drop | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_drop | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_drop | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_drop | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_drop | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_outcome | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_outcome | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_outcome | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_outcome | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| attempt_outcome | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_args | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_args | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_args | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_args | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_args | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_drop | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_drop | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_drop | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_drop | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_drop | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_token | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_token | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_token | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_token | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| call_token | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| model_drop | none-S-M-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_drop | A02-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_drop | A04-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_drop | A06-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_drop | A15-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_extra | none-S-M-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_extra | A02-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_extra | A04-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_extra | A06-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_extra | A15-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_input | none-S-M-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_input | A02-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_input | A04-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_input | A06-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_input | A15-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_output_sha | none-S-M-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_output_sha | A02-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_output_sha | A04-0/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_output_sha | A06-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| model_output_sha | A15-1/S-M/ | O-4.model | violation fail: O-4.model | replayed answer | flagged |
| outcome_result | none-S-M-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| outcome_result | A02-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| outcome_result | A04-0/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| outcome_result | A06-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| outcome_result | A15-1/S-M/ | O-4.calls | violation fail: O-4.calls | replayed answer | flagged |
| policy_action | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_action | A02-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_action | A04-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_action | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_action | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_drop | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_drop | A02-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_drop | A04-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_drop | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_drop | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_dup | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_dup | A02-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_dup | A04-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_dup | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_dup | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_result | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_result | A02-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_result | A04-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_result | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| policy_result | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| recon_retention | none-S-M-0/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| recon_retention | A02-0/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| recon_retention | A04-0/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| recon_retention | A06-1/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| recon_retention | A15-1/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| rule_version_null | none-S-M-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| rule_version_null | A02-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| rule_version_null | A04-0/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| rule_version_null | A06-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| rule_version_null | A15-1/S-M/ | O-4.policy | violation fail: O-4.policy | replayed answer | flagged |
| state_retention | none-S-M-0/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| state_retention | A02-0/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| state_retention | A04-0/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| state_retention | A06-1/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| state_retention | A15-1/S-M/ | O-4.retention | violation fail: O-4.retention | replayed answer | flagged |
| state_task_id | none-S-M-0/S-M/ | O-1.end_state | violation fail: O-1.end_state | replayed answer | flagged |
| state_task_id | A02-0/S-M/ | O-1.end_state | violation fail: O-1.end_state | replayed answer | flagged |
| state_task_id | A04-0/S-M/ | O-1.end_state | violation fail: O-1.end_state | replayed answer | flagged |
| state_task_id | A06-1/S-M/ | O-1.end_state | violation fail: O-1.end_state | replayed answer | flagged |
| state_task_id | A15-1/S-M/ | O-1.end_state | violation fail: O-1.end_state | replayed answer | flagged |
| status_done_to_doubt | none-S-M-0/S-M/ | O-1.end_state | violation fail: O-1.end_state | replayed answer | flagged |
| status_done_to_doubt | A02-0/S-M/ | O-1.end_state | violation fail: O-1.end_state | replayed answer | flagged |
| status_done_to_doubt | A06-1/S-M/ | O-1.end_state | violation fail: O-1.end_state | replayed answer | flagged |
| status_done_to_doubt | A15-1/S-M/ | O-1.end_state | violation fail: O-1.end_state | replayed answer | flagged |
| passthrough | none/S-M/0 | none (must pass) | pass | proxy applied 0x, errors 0 | correct-pass |
| passthrough | A04/S-M/0 | none (must pass) | pass | proxy applied 0x, errors 0 | correct-pass |
| model_used_false | none-S-M-0/S-M/ | O-4.model_used | pass | replayed answer | unsupported (clause listed in arm.json) |
| model_used_false | A02-0/S-M/ | O-4.model_used | pass | replayed answer | unsupported (clause listed in arm.json) |
| model_used_false | A04-0/S-M/ | O-4.model_used | pass | replayed answer | unsupported (clause listed in arm.json) |
| model_used_false | A06-1/S-M/ | O-4.model_used | pass | replayed answer | unsupported (clause listed in arm.json) |
| model_used_false | A15-1/S-M/ | O-4.model_used | pass | replayed answer | unsupported (clause listed in arm.json) |
| status_done_to_doubt | A04-0/S-M/ | O-2.memory | pass | replayed answer | unsupported (clause listed in arm.json) |

| control (snc, unmodified) | checker result | harness | detail | status |
|---|---|---|---|---|
| A01-0 | pass | ended | - | pass |
| A02-0 | pass | ended | - | pass |
| A02-1 | pass | ended | - | pass |
| A03-0 | violation fail: O-4.calls | ended | O-4.calls: T-F 'TF.e1' attempt 2: outcome 'effect', logged 'duplicate' | step-1 finding |
| A04-0 | pass | ended | - | pass |
| A04-1 | pass | ended | - | pass |
| A05-0 | pass | ended | - | pass |
| A06-0 | pass | ended | - | pass |
| A06-1 | pass | ended | - | pass |
| A07-0 | violation fail: O-4.calls | ended | O-4.calls: T-F 'TF.e1' attempt 2: outcome 'effect', logged 'duplicate' | step-1 finding |
| A08-0 | pass | ended | - | pass |
| A08-1 | pass | ended | - | pass |
| A11-0 | pass | ended | - | pass |
| A12-0 | pass | ended | - | pass |
| A13-0 | pass | ended | - | pass |
| A14-0 | pass | ended | - | pass |
| A14-1 | pass | ended | - | pass |
| A15-0 | violation fail: O-4.calls | ended | O-4.calls: T-I 'TI.e1' attempt 1: outcome 'duplicate', logged 'effect'; T-I 'TI.e1' attempt 2: outcome 'effect', logged 'duplicate' | step-1 finding |
| A15-1 | pass | ended | - | pass |
| A16-0 | pass | ended | - | pass |
| A16-1 | pass | ended | - | pass |
| A17-0 | pass | ended | - | pass |
| A17-1 | pass | ended | - | pass |
| none-S-M-0 | pass | ended | - | pass |

| L3 mutant (snc) | run | expected | checker result | mutation evidence | control fails | status |
|---|---|---|---|---|---|---|
| O-4-attempt-not-recorded | none-S-M-0 | O-4 (new clause) | pass | attempt-skipped marks 0, T-N requests 1 | - | unresolved (not exercised) |
| O-4-attempt-not-recorded | A02-0 | O-4 (new clause) | pass | attempt-skipped marks 0, T-N requests 1 | - | unresolved (not exercised) |
| O-4-attempt-not-recorded | A02-1 | O-4 (new clause) | pass | attempt-skipped marks 0, T-N requests 1 | - | unresolved (not exercised) |
| O-4-attempt-not-recorded | A04-0 | O-4 (new clause) | pass | attempt-skipped marks 0, T-N requests 1 | - | unresolved (not exercised) |
| O-4-attempt-not-recorded | A04-1 | O-4 (new clause) | pass | attempt-skipped marks 0, T-N requests 1 | - | unresolved (not exercised) |
| O-4-attempt-not-recorded | A06-0 | O-4 (new clause) | pass | attempt-skipped marks 0, T-N requests 1 | - | unresolved (not exercised) |
| O-4-attempt-not-recorded | A11-0 | O-4 (new clause) | pass | attempt-skipped marks 0, T-N requests 1 | - | unresolved (not exercised) |
| O-4-rule-version-not-recorded | none-S-M-0 | O-4 (new clause) | violation fail: O-4.policy | no-rule-version marks 1 | - | flagged |
| O-4-rule-version-not-recorded | A02-0 | O-4 (new clause) | violation fail: O-4.policy | no-rule-version marks 1 | - | flagged |
| O-4-rule-version-not-recorded | A04-0 | O-4 (new clause) | violation fail: O-4.policy | no-rule-version marks 1 | - | flagged |

