# PR #106 S6 REFINE round 1 (W=1): final review

Filed verbatim from cody's message in the project thread, 2026-11-11 20:06 UTC
(relayed because the reviewer's GitHub connector is read-only).

---

PR #106 FINAL REVIEW

Repository:

"codypendant/SNC-server-OS"

PR:

"#106 — S6 REFINE round 1 (W=1): PASS under unchanged criteria"

Reviewed head:

"7cdec23ac5d3d4076f87393a5ebaab7308a973fc"

Decision

PR #106: APPROVED.

REFINE ROUND 1: PASS.

MERGE AUTHORIZED.

The original frozen S6 result remains recorded as FAIL.

The accepted experimental sequence is:

S6 FAIL
→ causal characterization
→ REFINE #1 W=1
→ REFINE #1 PASS

No criterion was weakened.

No global default was changed.

No second rescue round has been consumed.

---

Important: CP-1A is NOT authorized yet

The current "bench/pilot.py" does not reproduce the accepted R640 checkpoint configuration.

For Profile A it currently passes only:

"--checkpoint-s 60"

It does not explicitly pass:

- R640 walk pacing values;
- target/max-age values;
- C3 writeback mode;
- REFINE #1 window W=1;
- sync-token configuration.

Running CP-1A now would therefore use compiled defaults, including the disproven W=4 behavior.

That would invalidate the campaign.

---

NEXT AUTHORIZED TASK — CP-1A CONFIGURATION CLOSURE

Create one small reviewed PR.

No benchmark run.

No product algorithm change.

The purpose is to make the CP-1A runner and profile guard reproduce the configuration that actually passed REFINE #1.

1. "bench/pilot_plan.json"

Add an explicit Profile-A checkpoint configuration.

Use exactly:

```
checkpoint_s          = 60
ckpt_pacing           = on
ckpt_walk_batch       = 16
ckpt_walk_min_us      = 20
ckpt_walk_max_us      = 65
ckpt_walk_ceiling_us  = 155
ckpt_walk_step_us     = 5
ckpt_target_ms        = 30000
ckpt_max_age_ms       = 60000
ckpt_io               = dsync
ckpt_window           = 1
ckpt_sync_tokens      = 0
```

Use a structured object rather than scattering independent constants if practical.

The existing "checkpoint_s" field may be folded into that structure only if all runner/tests/docs are updated atomically.

Do not change any other pilot parameter.

In particular do not change:

- objects;
- workload mix;
- CPU allocation;
- rates/grid algorithm;
- depth;
- warmup;
- windows;
- drain;
- reps;
- objective fraction;
- knee rule;
- seed;
- manifest.

---

2. "bench/pilot.py"

Every Profile A "snc_server" invocation must explicitly carry:

```
--checkpoint-s 60
--ckpt-pacing on
--ckpt-walk-batch 16
--ckpt-walk-min-us 20
--ckpt-walk-max-us 65
--ckpt-walk-ceiling-us 155
--ckpt-walk-step-us 5
--ckpt-target-ms 30000
--ckpt-max-age-ms 60000
--ckpt-io dsync
--ckpt-window 1
--ckpt-sync-tokens 0
```

The values must come from the committed plan rather than hidden hard-coded campaign constants.

Profile C must remain unchanged.

Do not generalize these R640 Profile-A values to Profile B or D without separate evidence.

---

3. "bench/profile_check.py"

Do not simply classify these checkpoint flags as harmless STRUCTURAL flags.

The guard must enforce the approved Profile-A values so a campaign cannot silently run another checkpoint configuration.

Add a Profile-A-specific fixed/required configuration mechanism.

Desired behavior:

- Profile A with all approved checkpoint flags and exact values: PASS.
- any approved flag missing: REFUSE;
- any value changed: REFUSE unless a future explicitly reviewed deviation says otherwise;
- W=4: REFUSE;
- old walk defaults: REFUSE;
- max-age 300000: REFUSE;
- pacing off: REFUSE;
- Profile C remains unaffected;
- do not impose these hardware-specific R640 values on B/D yet.

Keep the existing generic fixed-profile checks intact.

---

4. Tests

Update/add deterministic tests proving the campaign cannot regress to the old configuration.

At minimum:

Pilot command test

Generated Profile-A server command contains all eleven checkpoint flags with exact values.

Negative tests

Each of these must fail the guard independently:

- missing "--ckpt-window";
- "--ckpt-window 4";
- missing walk batch;
- max = 100 instead of 65;
- ceiling = 250 instead of 155;
- target/max-age mismatch;
- pacing off;
- wrong I/O mode;
- wrong sync-token value.

Profile C

Profile C continues to pass without durable checkpoint configuration.

Plan provenance

The generated command values come from "pilot_plan.json".

A mutation of the plan value must either change the command and then be rejected by the approved-plan hash/guard or otherwise be deterministically detected.

---

5. Documentation

Update:

- "docs/PILOT_PLAN.md"
- "docs/STATUS.md"
- "AGENTS.md"

Record:

- original S6 FAIL;
- REFINE #1 W=1 PASS;
- W=1 is now the approved R640 Profile-A checkpoint configuration;
- CP-1A cannot run until the pilot runner and guard carry the full configuration;
- no second rescue has been used.

Do not rewrite or erase the original S6 failure.

---

6. Plan hash / manifest discipline

Because "bench/pilot_plan.json" changes, its SHA-256 changes.

That is expected and must be handled explicitly.

Update every repository location that pins the approved pilot-plan hash, if such a pin exists.

Do not alter the approved manifest-e SHA.

The new plan hash must be committed and recorded before any CP-1A data.

---

Validation

Run:

- "test_pilot"
- "test_profile_check"
- any new checkpoint-plan test
- ordinary relevant Python test gate

A dry-run or command-construction proof should show Profile A using the exact accepted configuration.

Do not run the real pilot.

Return the PR for independent review.

---

After this closure

If the configuration-closure PR is correct and approved, the next sequence becomes:

merge configuration closure
→ freeze exact pilot HEAD + new pilot-plan hash
→ explicit CP-1A GO
→ rerun the full SNC-only CP-1A from the beginning

Not from the aborted step.

Not from a previous partial result.

No reuse of old manifest-d performance data.

---

Still unauthorized

Until the configuration closure is reviewed:

- CP-1A
- competitor benchmarks
- CRITERIA freeze
- S5
- B2
- REFINE round 2
- adaptive checkpoint-window logic
- global W default change
- Agent-SNC
- Rust
- DPDK/VPP

Final ruling:

PR #106 APPROVED.

REFINE #1 PASS ACCEPTED.

CP-1A remains on HOLD pending exact pilot-configuration closure.
