# PR #104 S6 R640 result: independent review

Filed verbatim from cody's message in the project thread, 2026-11-11 18:54 UTC
(relayed because the reviewer's GitHub connector is read-only).

---

PR #104 — S6 R640 RESULT REVIEW

Repository:

"codypendant/SNC-server-OS"

PR:

"#104 — S6 R640 checkpoint regression result: FAIL (rule 4, p99.9)"

Reviewed head:

"043d9880719d3460f32d190fda91a20948e5d15e"

Final ruling

PR #104 evidence: APPROVED.

MERGE AUTHORIZED.

S6 RESULT: FAIL.

This is a valid frozen experimental result, not a VOID and not a harness defect.

Do not reinterpret, weaken, or rerun the S6 result.

---

Provenance verified

PR #102 freeze closure merged.

PR #103 GO record merged as:

"e2aab620c7f5f80eddb6826ae6683ad258ef19a9"

The S6 session ran at exactly that HEAD.

Preflight:

- clean;
- frozen identities matched;
- machine/storage identity passed;
- binary hashes passed;
- no isolation violation.

Postflight:

- machine identity unchanged;
- storage state unchanged;
- binary identity unchanged.

Session:

- 15 planned reps;
- no VOID;
- no rerun;
- every P/H/C/C0 rep proved a checkpoint began inside its measured window.

Therefore the result is admissible S6 evidence.

---

Frozen S6 result

Verdict:

FAIL

Every P rep failed exactly one rule:

"4_p999"

Frozen rule:

"total p99.9 <= 10,000,000 ns"

Observed:

- P0: "14,155,776 ns"
- P1: "14,680,064 ns"
- P2: "14,680,064 ns"

All other P rules passed in all three reps.

P passed

- EXEC refusals = 0
- INDEX refusals = 0
- lateness ≈ 0.025%, below 0.1%
- p99 = 0.85–0.87 ms, below frozen reference limit 0.958 ms
- emits_peak <= 329
- checkpoint = 1.49–1.56 s
- no degraded checkpoint
- client rc = 0
- server rc = 0
- errors = 0
- stale = 0
- index errors = 0
- drained yes
- no failure marker

Capacity guard also passed:

- median C = 3,981,798 ops/s
- median C0 = 4,016,161 ops/s
- ratio ≈ 0.9914
- required >= 0.95

The FAIL therefore stands solely on p99.9 latency.

---

Root-cause review

Finding 1 — begin snapshot is not yet the primary target

All eight P shards show begin-snapshot durations of approximately:

"26.4–27.2 ms"

That is a real unbounded CPU pause and remains a known architectural issue.

However, the operation-class latency distribution does not look like a simple uniform snapshot pause.

N no-checkpoint

Approximate p99.9:

- read: 0.34 ms
- deposit: 1.15–1.18 ms
- transfer: 1.15–1.18 ms
- index: 0.80–0.84 ms

P checkpoint

Approximate p99.9:

- read: 2.9–3.4 ms
- index: 6.4–7.6 ms
- deposit: 17.3–17.8 ms
- transfer: 17.3–17.8 ms

The strongest tail increase is therefore in durability-sensitive mutations.

Do not authorize B2 from this evidence alone.

---

Finding 2 — checkpoint writeback / WAL interference is the primary measured bottleneck candidate

No-checkpoint N WAL EWMA peak:

"0.44–0.84 ms"

P WAL EWMA peak:

"6.9–7.4 ms"

P worst shard time above WAL-high:

approximately:

"0.8–1.1 s"

This coincides with the mutation/transfer tail increase.

---

Finding 3 — P's C3 chunk writes become bursty

Every shard writes 153 checkpoint chunks.

P chunk-write behavior:

- write EWMA approximately "9.7–35 ms"
- write max approximately "30–53 ms"
- "ckpt_window_full_steps" ranges from thousands to tens of thousands

The current per-shard write window is:

"W = 4"

The window is therefore repeatedly saturated in P.

By contrast:

H

- chunk write EWMA approximately 2.8–4.0 ms
- write max approximately 6.6–11.7 ms
- window-full steps = 0

C

- chunk write EWMA approximately 2.5–3.4 ms
- write max approximately 5.7–9.2 ms
- window-full steps = 0

H/C have slower checkpoint walks under greater foreground load, which naturally meters checkpoint chunk production.

P's lighter foreground load lets the checkpoint walk feed chunks faster, producing the more severe storage burst.

---

Architectural interpretation

The original checkpoint/backpressure design anticipated adaptive control of:

- walk budget;
- checkpoint write window.

The as-built controller controls the walk budget.

The S3 C3 write window remains fixed.

S6 has now exercised the previously unproven WAL-pressure path and exposed the likely gap:

walk pacing can reduce encoding CPU but cannot directly reduce checkpoint I/O concurrency once the fixed write window is driving storage pressure.

This is the primary hypothesis to test next.

---

Campaign decision

Do not proceed to CP-1A.

Do not change the frozen S6 threshold.

Do not simply rerun S6.

Do not implement B2 yet.

Do not implement S5/ioprio yet.

This is a candidate REFINE path, but first obtain direct causal evidence with the already-existing "--ckpt-window" control.

The experimental baseline allows at most two bounded rescue rounds.

The characterization below is diagnostic evidence used to choose REFINE #1; it is not itself a rewritten S6 PASS/FAIL run.

---

NEXT AUTHORIZED TASK — S6 write-window causal characterization

Create a preregistered, result-only characterization.

No product code change is authorized.

Use the same current binary as the S6 run.

Keep all P workload parameters unchanged except "--ckpt-window".

Cells

W1

Exact P workload, but:

"--ckpt-window 1"

3 reps.

W2

Exact P workload, but:

"--ckpt-window 2"

3 reps.

The existing frozen S6 P reps at:

"--ckpt-window 4"

are the W4 reference.

Do not rerun W4 merely to seek a better result.

Keep unchanged

- same server binary
- same net_bench
- same R640
- same manifest e state
- same CPU allocation
- same NUMA binding
- same scratch/output isolation
- 5M objects
- Profile-A mix
- 436,646 ops/s
- open-loop depth 1024
- 10 s warmup
- 60 s measured window
- 10 s drain
- checkpoint every 60 s
- pacing 20/65/155/5/batch16
- target 30 s
- hard max 60 s
- 100 ms GAUGES/HEALTH sampling

No S6 rule is modified.

---

Characterization outputs

For every W1/W2 rep record:

Client

- total p99
- total p99.9
- max
- per-kind p99 and p99.9
- refusals
- lateness
- drained/error state

WAL/controller

- WAL EWMA peak
- time above WAL-high
- backlog peaks
- walk-budget trace/range
- catch-up time

Checkpoint write path

Per shard:

- "ckpt_write_lat_ewma_us"
- "ckpt_write_lat_max_us"
- "ckpt_window_full_steps"
- "ckpt_chunks"
- snapshot duration
- walk duration
- total checkpoint duration

Machine

Pre/post identity exactly as S6.

---

Characterization interpretation frozen before data

The purpose is causal discrimination, not choosing a favorable PASS threshold.

Supports write-window hypothesis

Evidence supports the fixed-window hypothesis if lowering W produces a coherent reduction in:

1. checkpoint write latency;
2. WAL EWMA peak/time-high;
3. mutation/transfer tail latency;

while checkpoint completion remains comfortably inside the existing 60 s hard maximum.

The evidence should be monotonic or directionally coherent:

"W1 <= W2 <= W4"

for storage pressure/tail metrics, allowing normal measurement noise.

Does not support it

If W1/W2 materially reduce checkpoint write latency but mutation p99.9 remains near the W4 failure, the write window is not sufficient to explain the tail.

If neither write latency nor WAL pressure materially changes, reject the hypothesis.

If checkpoint duration becomes pathological, record it; do not widen the hard maximum.

---

After characterization

STOP and return for independent review.

Do not automatically select W1 or W2.

Do not modify the production controller yet.

Do not rerun full S6 yet.

The review will choose one of:

1. REFINE #1: checkpoint write-window control
   - likely fixed lower W or adaptive window control under WAL pressure;
2. S5 I/O-priority characterization;
3. B2 begin-snapshot work;
4. another bounded hypothesis if the evidence demands it.

The rescue implementation, if authorized, must be one bounded slice.

After that slice is independently reviewed, only the affected S6 cells are rerun under the unchanged criteria, per the experimental REFINE rule.

---

PR #104 merge state

PR #104 contains result evidence only.

It may merge.

Do not describe its merge as a successful S6.

Record:

S6 FAIL — rule 4 p99.9.

The failure is accepted evidence and is now the basis for the bounded REFINE investigation.

---

Standing rules:

Always verify, never assume.

Do not tune the criterion after seeing the result.

Do not fix the most obvious-looking mechanism before measuring causality.

The first rescue must address one measured bottleneck only.
