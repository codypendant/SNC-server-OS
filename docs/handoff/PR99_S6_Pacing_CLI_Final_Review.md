# PR #99 S6 pacing CLI: final review

Filed verbatim from cody's message in the project thread, 2026-11-11 16:26 UTC
(ChatGPT's review, relayed because its GitHub connector is read-only).

---

PR #99 FINAL REVIEW

Decision

PR #99: APPROVED.

S6 pacing CLI plumbing: COMPLETE.

MERGE AUTHORIZED.

S6: NOT GO.

Reviewed head:

"8d40447787ee105555d7a783c585a2b971228c44"

---

Scope review

The PR stays within the authorized slice.

Changed behavior is limited to exposing existing checkpoint-walk configuration through "snc_server".

The PR adds:

- "--ckpt-walk-batch N"
- "--ckpt-walk-min-us N"
- "--ckpt-walk-max-us N"
- "--ckpt-walk-ceiling-us N"
- "--ckpt-walk-step-us N"

The values map to existing "Engine::Config" fields.

Verified:

- no "src/" changes;
- no "include/" changes;
- no pacing-algorithm change;
- no checkpoint-ordering change;
- no storage-path change;
- no pressure-threshold change;
- no compiled-default change;
- no S6 performance/result data.

The characterized "ckpt_walk_cost" binary remains byte-identical:

"4707dac135a6fc72c41a712075678aa24ccbf7ad74d546962623026584b5d2d2"

Therefore the PR #98 R640 pacing measurements remain applicable.

---

CLI parsing and validation

Approved.

Time values accept:

"1 .. 60,000,000 us"

which matches the Engine's 60-second pacing bound.

Batch accepts:

"1 .. UINT32_MAX"

Parsing rejects:

- zero;
- negative values;
- "+" prefix;
- leading spaces;
- decimal values;
- malformed strings;
- empty values;
- numeric overflow;
- time values above 60 seconds;
- batch values above "u32".

The effective configuration is checked before Engine construction for:

"min <= max <= ceiling"

This correctly handles both:

- complete multi-flag configurations;
- a single new flag that conflicts with unchanged defaults.

Invalid new pacing CLI configurations exit cleanly with:

"rc = 2"

instead of escaping into an uncaught "std::invalid_argument".

---

Approved R640 S6 values

The server accepts the previously approved R640 profile:

- batch = "16"
- min = "20 us"
- max = "65 us"
- ceiling = "155 us"
- step = "5 us"
- target = "30000 ms"
- max age = "60000 ms"

No default is changed globally.

That is the correct design: the R640 profile is explicit campaign configuration rather than a hardware-specific mutation of SNC's compiled defaults.

---

Startup provenance

Approved.

Every successful server start now prints the effective checkpoint pacing configuration:

"ckpt_pacing: pacing= batch= min_us= max_us= ceiling_us= step_us= update_us= target_ms= max_age_ms="

This is required evidence for S6 and prevents silent dependence on assumed defaults.

Keep this startup line.

---

Tests and mutation coverage

Accepted evidence:

- targeted CLI test passes;
- defaults verified unchanged;
- approved R640 configuration verified;
- each flag independently verified;
- 56 invalid cases cleanly rejected with exit code 2;
- upper bounds accepted;
- 5 / 5 negative mutations caught;
- RelWithDebInfo full suite: 48 / 48;
- ASan+UBSan full suite: 48 / 48;
- zero sanitizer reports.

Mutation coverage demonstrates that the important new behavior is actually guarded:

- removing order validation is caught;
- accepting zero is caught;
- removing the 60-second bound is caught;
- parsing-but-not-applying "max" is caught;
- weakening digits-only parsing is caught.

---

TSan decision

TSan rerun is NOT required for PR #99.

The waiver is accepted.

Reason:

The changed mechanism consists of:

- main-thread command-line parsing before Engine construction;
- assignment into local "Engine::Config";
- pre-construction validation;
- one startup "printf".

No new shared state, concurrent access, synchronization primitive, thread lifecycle, or memory-order behavior is introduced.

A full TSan run would not materially exercise the changed mechanism beyond what the existing parser tests already establish.

This waiver applies only to PR #99's narrow parser/configuration slice.

---

Post-merge next authorized task

After PR #99 is merged:

Build the S6 runner and finalize preregistration

This is a separate PR.

Do not run S6 while implementing it.

Required deliverables:

Runner

Create:

"bench/s6.py"

Use:

- corrected diag3 run discipline;
- "bench/pilot.py" device/scratch isolation;
- current GAUGES/HEALTH paths.

Required runner behavior

The runner must:

1. refuse a dirty git tree;
2. record exact HEAD;
3. record S6 "snc_server" SHA-256;
4. record the one common current "net_bench" SHA-256;
5. record pre-S1 server SHA-256;
6. verify manifest-e machine state before and after the session;
7. enforce data/scratch/output disk separation;
8. run exactly the preregistered cells;
9. enforce exact PASS/FAIL/VOID arithmetic;
10. perform only explicitly allowed VOID reruns;
11. preserve all raw output;
12. obtain final GAUGES/HEALTH before shutdown;
13. wait for a started checkpoint to finish or hit the hard maximum;
14. verify P/H checkpoints began inside the measured window;
15. support dry-run mode that prints every command without producing load.

Controller sampling

Sample GAUGES/HEALTH every:

100 ms

during the current-server cells.

Pin:

- GAUGES/HEALTH sampler → housekeeping CPU 0;
- disk observer → housekeeping CPU 36.

Preserve timestamped raw samples.

Use the samples to derive:

- WAL EWMA peak;
- time above WAL-high;
- backlog peak;
- time above 25%;
- time above 50%;
- walk budget over time;
- catch-up state;
- phase timing.

Calculate time-above-threshold using timestamps, not merely sample count.

Do not invent zeros for fields that do not exist on the pre-S1 server.

---

Capacity comparator rule

Approved pre-S1 server commit:

"389b6234855082a4591c6677d0a303c3add3319d"

This was verified as:

- merge of PR #71;
- exact base of PR #72;
- therefore the tip immediately before S1.

For capacity comparison:

- current C cell uses final S6 "snc_server";
- C0 uses the "389b623" "snc_server";
- both must use the same final/current S6 "net_bench" binary.

Do not use the old "389b623" load generator.

Before S6 GO, perform a dev-machine compatibility check showing that the current "net_bench" can drive the "389b623" single-node Profile-A server workload.

If incompatible:

STOP and return for review.

Do not silently change the comparator.

---

Frozen preregistration decisions

Carry forward exactly:

R640 pacing

- batch: 16
- min: 20 us
- max: 65 us
- ceiling: 155 us
- step: 5 us
- pace update: 1 ms

Checkpoint

- target: 30,000 ms
- hard max age: 60,000 ms
- warn: 80%
- checkpoint interval: 60 s

Primary S6 cell

- 436,646 ops/s
- open loop
- Profile A
- 60 s measured window
- 3 reps
- checkpoint every 60 s

p99 reference

Use:

same-SHA no-checkpoint median

Do not use paired-reference interpretation.

Comparator cells

- N: 436,646, no checkpoint, 3 reps
- P: 436,646, checkpoint, 3 reps
- H: 2,183,233, checkpoint, 3 reps
- C: current closed-loop checkpoint capacity
- C0: pre-S1 "389b623" server closed-loop checkpoint capacity

Primary PASS rules

Every P rep must satisfy:

- zero refusals;
- "late_over_1ms <= 0.1% scheduled_in_window";
- total p99 <= 1.5 × N median;
- total p99.9 <= 10 ms;
- "emits_peak < 1024" on every shard;
- checkpoint completes within 60,000 ms;
- no degraded checkpoint;
- no correctness/durability error;
- drained successfully.

Capacity guard:

current median checkpoint capacity >= 95% of pre-S1 median.

---

Runner tests required

Pin deterministic tests for:

- p99 1.5× boundary;
- lateness 0.1% boundary;
- p99.9 10 ms boundary;
- backlog 1024 boundary;
- capacity 95% boundary;
- refusal rule;
- correctness/drained rule;
- N-reference unusable VOID;
- checkpoint-outside-window VOID;
- machine/build drift VOID;
- one-rerun maximum for allowed VOID cases;
- no rerun for genuine FAIL;
- median calculation;
- timestamp-based controller duration calculations;
- old-server missing-gauge handling.

---

Final preregistration freeze

Before asking for S6 GO, "docs/S6_PREREG.md" must contain the final exact:

- merged S6 HEAD SHA;
- current server SHA-256;
- common current "net_bench" SHA-256;
- pre-S1 server SHA-256;
- manifest-e SHA-256;
- runner path;
- runner validation evidence;
- approved pacing values;
- 30 s target;
- 60 s max age;
- 100 ms controller sample interval;
- comparator commit;
- exact cells;
- exact PASS/FAIL/VOID rules.

Then return the runner/preregistration PR for independent review.

No S6 workload may run before explicit GO.

---

Still forbidden

Until that review:

- no S6 performance/result run;
- no CP-1A rerun;
- no PostgreSQL/Redis/other comparator campaign;
- no CRITERIA freeze;
- no S5/ioprio;
- no B2 incremental snapshot;
- no Agent-SNC implementation;
- no Rust rewrite;
- no DPDK/VPP.

Sequence:

merge #99
→ S6 runner + final preregistration PR
→ independent review
→ explicit S6 GO
→ S6 run
→ S6 result review
→ then clean CP-1A from the beginning

Standing rules:

Always verify, never assume.

Fix confirmed defects on site.

Correctness before performance.

Never tune a pass/fail parameter after seeing the result it judges.

Use one load generator when comparing old and new server capacity.
