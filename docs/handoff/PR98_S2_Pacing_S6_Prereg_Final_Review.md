# PR #98 final review

Filed verbatim from cody's upload in the project thread, 2026-11-11 15:04 UTC.

---

# PR #98 Final Review — R640 S2 Pacing Re-Derivation and S6 Preregistration

Date: 2026-11-11  
PR: #98 — `R640 S2 pacing re-derivation + S6 preregistration (for review, NOT GO)`  
Reviewed head: `72220261906b3fb3426d9b8c3c4bcd21391131ac`  
Base: `0dbd4c77b993572fb7b771fa44e7520c3197fe1b` (merged PR #97 main)

## Decision

**PR #98 characterization: APPROVED.**

**R640 S2 pacing re-derivation: APPROVED.**

**S6 preregistration direction: APPROVED WITH THE DECISIONS BELOW.**

**PR #98 may merge as evidence + preregistration framework.**

**S6: NOT GO.**

No S6 performance/result cells may run yet.

The next authorized work is the minimal configuration-plumbing PR described in section 7, followed by the S6 runner/preregistration-finalization PR described in section 8.

---

## 1. R640 characterization — accepted

The characterization was performed against merged post-PR97 main:

`0dbd4c77b993572fb7b771fa44e7520c3197fe1b`

with a fresh RelWithDebInfo build.

The evidence establishes:

- manifest-e hardware state;
- governor `performance` on all 72 CPUs;
- `kernel.numa_balancing=0`;
- 12 × 32 GB DIMMs, A1-A6 and B1-B6;
- same ARFX3840S5xnFTRI data SSD;
- same ext4 filesystem as manifest d, not a recreated filesystem;
- filesystem inode tables fully initialized;
- no ext4 lazy-init thread;
- no idle write activity during the check.

The unresolved disappearance of the old 2.2 GB is correctly left as **cause unknown**. No unsupported causal claim is needed for the current measurements.

---

## 2. Measurement method — accepted

`bench/ckpt_walk_cost` by itself could not cleanly isolate walk CPU cost because:

1. S3's dsync write window causes the walk phase wall clock to include I/O waiting;
2. one-CPU placement exposes process/I/O-worker preemption in maximum step latency.

The follow-up bpftrace uprobes timed the unchanged binary directly and are acceptable for deriving the pacing envelope.

Important properties:

- no product code was changed for the measurement;
- the exact `ckpt_walk_cost` binary was probed;
- all 54 probe runs returned rc=0;
- three process repetitions were retained;
- the probe overhead is approximately 1-2 µs per call, making the measured batch cost an upper bound rather than a favorable underestimate;
- run-to-run p50 variation was below 1%.

The summarizer correctly aggregates all three processes × three checkpoints per process.

---

## 3. Measured R640 walk costs — accepted

Primary L2 placement measurements:

- legacy 256-ID visit:
  - p50 = **171.6 µs**
  - p99 = **187.7 µs**
- legacy whole step:
  - p50 = 178.0 µs
  - p99 = 194.3 µs
- batch-16 visit:
  - p50 = **13.3 µs**
  - p99 = **16.4 µs**
- approximate walk cost:
  - **670 ns / ID**

The old S2 assumption that R640 walk work might be around 80-100 ns/ID was incorrect. The actual image/encode walk is much more expensive, with `encode_record` the dominant sampled symbol.

The L0 long-tail steps are correctly classified as placement/preemption effects rather than pacing-budget work. They remain a recorded S6 risk, not something to tune away after seeing S6 data.

---

## 4. Derived R640 pacing values — APPROVED

Use for S6:

| Field | Approved S6 value |
|---|---:|
| `ckpt_pacing` | on |
| `ckpt_walk_batch` | **16** |
| `ckpt_walk_min_ns` | **20,000** |
| `ckpt_walk_max_ns` | **65,000** |
| `ckpt_walk_ceiling_ns` | **155,000** |
| `ckpt_walk_step_ns` | **5,000** |
| `ckpt_pace_update_ns` | **1,000,000** |
| WAL high / low | **200% / 125%**, inherited hypothesis |
| backlog high / low | **50% / 25%**, inherited hypothesis |

The derivation is accepted.

Inputs:

- `T256 = 171.6 µs` — legacy 256-ID p50;
- `b = 16.4 µs` — batch-16 p99, including probe overhead.

Rules:

### Ceiling

`ceiling + b <= T256`

`171.6 - 16.4 = 155.2 µs`

Approved:

`ceiling = 155 µs`

Worst routine catch-up step:

`155 + 16.4 = 171.4 µs`

approximately 1.00 × the legacy p50.

### Normal maximum

`max + b <= T256 / 2`

`85.8 - 16.4 = 69.4 µs`

rounded down conservatively:

`max = 65 µs`

Worst routine normal step:

`65 + 16.4 = 81.4 µs`

approximately 0.47 × the legacy p50.

### Minimum

A 20 µs budget independently produced approximately two batches per step; 10 µs produced one.

Approved:

`min = 20 µs`

### Step

`(65 - 20) / 8 = 5.6 µs`

Rounded down:

`step = 5 µs`

Nine update intervals from min to max are acceptable.

The dev-VM 100 µs max and 250 µs ceiling are **rejected for the R640 S6 configuration**.

No S6 result was used to choose these values.

---

## 5. S6 preregistration decisions

### Decision A — p99 reference

**APPROVED as written.**

Use:

**same-SHA no-checkpoint median**

for all three primary P reps.

Do not use paired references and do not change the rule after any S6 result exists.

### Decision B — checkpoint target and hard maximum

Approve:

- `ckpt_target_ms = 30,000`
- `ckpt_max_age_ms = 60,000`
- `ckpt_warn_pct = 80`

The 30 s target remains a pacing goal, not a pass threshold by itself.

The 60 s hard maximum is accepted because the checkpoint interval is 60 s. A checkpoint that cannot complete within one full interval cannot keep the configured schedule and causes retained work to accumulate. The previous 300 s value was explicitly a placeholder.

Freeze 60,000 ms before S6 data. Do not widen it after a failing S6 run.

### Decision C — pre-S1 comparator

**`389b6234855082a4591c6677d0a303c3add3319d` is APPROVED.**

Verified:

- it is the merge of PR #71;
- it is exactly PR #72's base;
- therefore it is the tip of `main` immediately before S1.

For the capacity guard, however, isolate the server change:

- C uses the final S6 `snc_server`;
- C0 uses the `389b623` `snc_server`;
- **both C and C0 use the same final S6 `net_bench` binary and the same final S6 harness**.

Do not use `389b623`'s old `net_bench` for C0. That would confound engine capacity with load-generator changes.

Before S6 GO, perform a non-campaign compatibility check on the dev machine that the final S6 `net_bench` can drive the `389b623` server for the required single-node Profile-A workload. If it cannot, STOP and return for review rather than silently changing the comparator.

Record the old-server binary SHA and the one common current-client SHA.

### Decision D — S6 rate

Keep 436,646 ops/s exactly as preregistered.

It originated from manifest-d CP-1A, but it is intentionally a regression cell rather than a newly selected manifest-e capacity point. Do not retune it using manifest-e results before S6.

---

## 6. Controller trace sampling — REQUIRED

The DESIGN-R S6 plan requires controller evidence, including:

- WAL-latency EWMA peak;
- time above the high WAL threshold;
- backlog peak;
- time above 25% and 50%;
- checkpoint phase timings;
- walk-budget trace;
- catch-up behavior.

End-of-rep gauges are not sufficient to reconstruct the time-above-threshold or budget trace.

Therefore the S6 runner **must sample GAUGES/HEALTH during the run**.

Approved cadence:

**100 ms**

Requirements:

- pin the gauge sampler to housekeeping CPU 0;
- pin the disk observer to housekeeping sibling CPU 36, matching the previous diag3 discipline;
- use the same 100 ms admin-request cadence for all S6 cells, including N, P, H, C, and C0 wherever the old server supports the request;
- the parser must tolerate fields absent from the pre-S1 server without inventing zeros for semantically nonexistent fields;
- preserve raw timestamped samples;
- calculate time-above-threshold from timestamps, not sample count alone;
- report gaps or failed samples explicitly.

The overhead is expected to be small: S0 designed GAUGES for nonblocking reads and 100 ms is substantially lighter than the 10 ms diagnostic rate considered in the S0 analysis.

Because the same cadence is used in the reference and primary current-SHA cells, instrumentation is not selectively applied only to the checkpoint arm.

---

## 7. Next PR: S6 pacing CLI plumbing — AUTHORIZED

Create a **small standalone PR**.

Do not include S6 result data.

Scope:

`tools/snc_server.cpp`, targeted tests, usage/help text, and handoff/status documentation only.

Do **not** change:

- `src/`
- `include/`
- pacing algorithm;
- Engine defaults;
- controller thresholds;
- checkpoint ordering;
- storage path.

Expose these existing `Engine::Config` fields:

- `--ckpt-walk-batch N`
- `--ckpt-walk-min-us N`
- `--ckpt-walk-max-us N`
- `--ckpt-walk-ceiling-us N`
- `--ckpt-walk-step-us N`

Compiled defaults stay exactly as they are today.

### Parsing/validation

- batch: positive `u32`, reject 0 and overflow;
- time values: positive microseconds, safely converted to ns;
- reject values over 60 seconds;
- enforce:
  `min <= max <= ceiling`;
- reject malformed/negative/overflow values;
- invalid CLI configuration exits **2**, not an uncaught exception.

Do not duplicate more Engine validation than necessary.

The server should print the effective checkpoint pacing configuration at startup so every S6 server log records at least:

- pacing on/off;
- batch;
- min_us;
- max_us;
- ceiling_us;
- step_us;
- update_us;
- target_ms;
- max_age_ms.

### Tests

Add deterministic command-line tests covering:

- each valid flag;
- 20 / 65 / 155 / 5 / batch 16 combination;
- zero;
- negative;
- malformed input;
- overflow;
- `max < min`;
- `ceiling < max`;
- values over the 60 s Engine bound;
- unchanged defaults when no new flags are supplied;
- startup output contains the effective values.

Return the PR for independent review.

**Do not run S6.**

---

## 8. Following PR: S6 runner + preregistration finalization — AUTHORIZED AFTER SECTION 7 MERGES

After the CLI-plumbing PR is approved and merged, build a separate harness PR.

Add:

- `bench/s6.py`
- deterministic tests for every rule boundary and VOID rule;
- finalized `docs/S6_PREREG.md`;
- status/handoff updates.

Base the runner on:

- diag3's corrected run discipline;
- `bench/pilot.py` scratch isolation/device resolver;
- current admin GAUGES/HEALTH paths.

No product behavior change in this PR.

### Runner requirements

The runner must:

1. refuse a dirty git tree;
2. record exact HEAD;
3. record final S6 `snc_server` and common `net_bench` SHA-256 values;
4. record pre-S1 server SHA-256;
5. verify manifest-e machine state before and after;
6. enforce scratch/data/output disk separation;
7. execute exactly the preregistered cells and order;
8. apply the exact PASS/FAIL/VOID arithmetic from `S6_PREREG.md`;
9. perform only the explicitly preregistered reruns for VOID conditions;
10. sample GAUGES/HEALTH every 100 ms as section 6 above requires;
11. preserve all raw logs and samples;
12. read final gauges before shutdown;
13. wait for a started checkpoint to finish or hit the hard maximum before stopping the server;
14. verify a P/H checkpoint actually began inside the measured window;
15. use the same final S6 `net_bench` for both C and C0;
16. produce a dry-run mode printing every command without load.

Tests must pin:

- exact p99 3/2 boundary;
- exact lateness 0.1% boundary;
- p99.9 10 ms boundary;
- backlog 1024 boundary;
- 95% capacity boundary;
- no-refusal rule;
- correctness/drained rule;
- checkpoint-in-window VOID;
- unusable N reference VOID;
- machine/build drift VOID;
- one rerun maximum for permitted VOID cases;
- no rerun for a genuine FAIL;
- median selection for N and capacity;
- timestamp-based controller time-above-threshold calculations;
- old-server missing-gauge-field handling.

### Final preregistration freeze

After this runner PR merges, update `docs/S6_PREREG.md` with:

- exact merged S6 HEAD SHA;
- exact server/client binary hashes;
- exact pre-S1 server binary hash;
- approved 20/65/155/5/batch-16 pacing values;
- target 30,000 ms;
- max age 60,000 ms;
- same-SHA no-checkpoint p99 reference;
- comparator `389b623`;
- 100 ms controller-sampling cadence;
- exact runner path and its test evidence.

Then return for **final independent review / explicit S6 GO**.

No S6 cell may execute before that GO.

---

## 9. PR #98 merge status

PR #98 contains evidence and a draft preregistration, not product behavior.

It may merge after this review is filed.

Its purpose is to preserve:

- the R640 measurement evidence;
- the approved R640 pacing derivation;
- the preregistration framework;
- the fact that no S6 data existed when the values and rules were chosen.

Do not describe merge of PR #98 as S6 authorization.

---

## 10. Still not authorized

- S6 performance/result cells;
- CP-1A rerun;
- competitor/comparator systems;
- CRITERIA freeze;
- S5/ioprio;
- B2 incremental begin snapshot;
- Agent-SNC implementation;
- Rust rewrite;
- DPDK/VPP.

Sequence:

PR #98 merge  
→ pacing CLI-plumbing PR  
→ review/merge  
→ S6 runner + prereg-finalization PR  
→ review/merge  
→ **explicit S6 GO**  
→ S6 regression  
→ S6 review  
→ only then clean CP-1A from the beginning.

Standing rules:

**Always verify, never assume.**

**Fix confirmed defects on site.**

**Correctness before performance.**

**Never tune a pass/fail parameter after seeing the result it judges.**

**Use one load generator when comparing old and new server capacity.**
