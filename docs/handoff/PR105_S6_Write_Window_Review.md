# PR #105 S6 write-window characterization: final review (REFINE round 1 authorized)

Filed verbatim from cody's message in the project thread, 2026-11-11 19:31 UTC
(relayed because the reviewer's GitHub connector is read-only).

---

PR #105 FINAL REVIEW — S6 WRITE-WINDOW CHARACTERIZATION

Repository:

"codypendant/SNC-server-OS"

PR:

"#105 — S6 write-window causal characterization (W1/W2)"

Reviewed head:

"10ea86ca61769b51e7f082aa39b4f490f0f5ab27"

Decision

PR #105: APPROVED.

MERGE AUTHORIZED.

Fixed checkpoint write-window pressure is accepted as the named bottleneck for REFINE round 1.

REFINE ROUND 1 AUTHORIZED.

The original S6 result remains:

FAIL — rule 4 p99.9.

The diagnostic W1 results do not retroactively make S6 pass.

---

Characterization validity

Verified:

- preregistration committed before W1/W2 data at "c13153f";
- no product-code change;
- exact frozen S6 binaries;
- same R640;
- same manifest e;
- same storage state;
- same CPU/NUMA allocation;
- same workload;
- same pacing;
- same checkpoint interval;
- same sampling;
- W4 was read from original S6 evidence, not rerun;
- W1/W2 server commands differ from P only by "--ckpt-window N";
- preflight clean;
- postflight unchanged;
- 6/6 reps completed;
- no VOID;
- no rerun;
- no refusals or correctness failures.

---

Causal result

The preregistered hypothesis is SUPPORTED.

W4 → W2 → W1

Checkpoint write latency

W4 EWMA:

"9.7–35.0 ms"

W2:

"6.2–16.2 ms"

W1:

"3.2–7.8 ms"

Checkpoint write max

W4:

"30.1–52.9 ms"

W2:

"22.0–31.3 ms"

W1:

"11.1–16.3 ms"

WAL EWMA peak

W4:

"6.9–7.4 ms"

W2:

"6.5–7.0 ms"

W1:

"4.4–4.6 ms"

Time above WAL-high

W4:

"0.8–1.1 s"

W2:

"0.6–0.8 s"

W1:

"0"

Deposit/transfer p99.9

W4:

"17.3–17.8 ms"

W2:

"16.8–17.3 ms"

W1:

"12.1–12.6 ms"

Total p99.9

W4:

"14.16–14.68 ms"

W2:

"13.89–14.42 ms"

W1:

"9.70 ms / 9.70 ms / 9.70 ms"

The frozen S6 limit remains:

"10.0 ms"

The W1 numbers are diagnostic only and are not counted as a passed S6 result.

---

Interpretation

The evidence establishes that the fixed C3 write window at W=4 materially contributes to the S6 mutation tail.

W2 reduces raw checkpoint-write latency but does not reduce WAL pressure or the application tail enough.

W1 crosses the important operating boundary:

- WAL no longer enters the configured high-pressure state;
- mutation tail materially improves;
- total p99.9 drops below the existing frozen objective;
- checkpoint completion does not materially regress.

Therefore the measured bottleneck for rescue round 1 is:

checkpoint write concurrency against foreground WAL durability.

---

What remains unresolved

W1 does not eliminate all checkpoint tail effects.

Compared with N:

- mutation p99.9 remains roughly 12 ms vs ~1.2 ms;
- WAL EWMA remains roughly 4.5 ms vs <1 ms;
- read/index tails remain elevated;
- max latency remains about 49 ms.

The begin snapshot remains approximately 27 ms and may contribute to those residual tails.

This does not block REFINE #1 because the rescue criterion is the existing frozen S6 criterion, not equivalence to N.

Do not authorize B2 from this characterization.

---

REFINE ROUND 1 — FIX

Use the existing tested configuration:

"--ckpt-window 1"

No engine algorithm change is required.

Do not change the global compiled default yet.

Do not build adaptive-window logic yet.

The rescue is intentionally narrow:

S6/campaign R640 checkpoint configuration changes from W=4 to W=1.

This uses an existing S3 mechanism whose W=1 behavior already has deterministic correctness coverage.

---

Why fixed W=1 instead of adaptive window control

The campaign rule requires the smallest bounded rescue addressing one measured bottleneck.

W=1 already:

- exists;
- is configurable;
- is tested;
- preserves checkpoint semantics;
- directly addresses the measured bottleneck;
- produced the preregistered causal response.

An adaptive write-window controller would be a new algorithm and a larger rescue than the evidence currently requires.

Adaptive control may be revisited later if broader workloads show W=1 is too restrictive.

Do not build it in REFINE #1.

---

REFINE #1 PREREGISTRATION

Before running the rescue cells, commit a new rescue preregistration and runner/overlay.

Do not edit or overwrite the original S6 evidence.

Suggested result path:

"bench/results/s6-refine1-r640-20261111/"

Fixed rescue configuration

Current-server checkpoint cells add:

"--ckpt-window 1"

Everything else stays exactly frozen.

No criterion changes.

---

Cells to rerun

Per the Experimental Baseline REFINE rule:

«rerun only affected cells under unchanged criteria»

The affected current-server checkpoint cells are:

P1

Original S6 P workload with W=1.

3 reps.

This is the primary rescue PASS/FAIL cell.

H1

Original H workload with W=1.

3 reps.

Reported only, same as original S6 H.

C1

Original current-server closed-loop checkpoint-capacity cell with W=1.

3 reps.

Capacity numerator.

---

References reused unchanged

Do not rerun unaffected cells merely to seek better reference values.

N

Reuse original frozen S6 N reps.

Frozen p99 reference remains:

"638,976 ns"

Therefore the original p99 limit remains:

"958,464 ns"

C0

Reuse original frozen S6 pre-S1 C0 reps.

Frozen C0 median remains:

"4,016,161 ops/s"

Therefore capacity guard remains:

"median C1 >= 0.95 × 4,016,161"

No new baseline selection.

No new threshold selection.

---

P1 frozen pass rules

Every P1 rep must satisfy the original S6 criteria:

1. zero EXEC refusals;
2. zero INDEX refusals;
3. "late_over_1ms × 1000 <= scheduled_in_window";
4. total p99 <= "958,464 ns";
5. total p99.9 <= "10,000,000 ns";
6. "emits_peak < 1024" on every shard;
7. checkpoint completes < 60,000 ms;
8. no degraded checkpoint;
9. client rc = 0;
10. errors = 0;
11. stale = 0;
12. index errors = 0;
13. drained yes;
14. server alive through measured run and clean exit;
15. no failure marker.

No threshold may change.

---

Capacity guard

Use:

"median(C1) >= 95% × 4,016,161"

and require rule-7 correctness for all C1 reps.

The original C0 evidence remains the denominator.

Do not replace it with a new C0 run.

---

H1

Run and preserve the full diagnostic evidence.

H1 remains reported, not judged.

Its purpose is to show whether W=1 behaves pathologically at the high offered-load diagnostic point.

Do not turn H into a new pass/fail criterion.

---

Rescue run mechanics

Use the same frozen:

- current "snc_server"
- current "net_bench"
- R640
- manifest e
- data SSD
- machine checks
- scratch isolation
- CPU allocation
- NUMA binding
- pacing values
- 100 ms sampler
- no-catch-up sampling
- checkpoint-window proof
- VOID/rerun policy

The only functional difference for P1/H1/C1 is:

"--ckpt-window 1"

---

Rescue result rules

PASS REFINE #1

REFINE #1 succeeds only if:

- all three P1 reps pass every original P rule;
- the capacity guard with C1 against frozen C0 passes;
- no session VOID;
- machine/build identities remain valid.

H1 is informational.

FAIL REFINE #1

If any genuine P1 criterion fails or the capacity guard fails:

REFINE #1 FAILS.

Do not change W.

Do not rerun a genuine FAIL.

Do not weaken any criterion.

Return for review.

That would leave only one bounded rescue round available.

---

No automatic CP-1A

Even if REFINE #1 passes:

STOP.

Commit the result evidence and open the result-review PR.

Do not immediately rerun CP-1A.

The rescue result must first receive independent review.

---

Still unauthorized

During REFINE #1:

- adaptive window controller;
- changing global default W;
- S5/ioprio;
- B2 incremental begin snapshot;
- S6 threshold changes;
- pacing retuning;
- CP-1A;
- competitor runs;
- CRITERIA freeze;
- Agent-SNC;
- Rust;
- DPDK/VPP.

---

Experimental accounting

This is:

REFINE ROUND 1 OF MAXIMUM 2.

Do not count the W1/W2 causal characterization itself as a rescue round.

The rescue round begins with the formal W=1 affected-cell rerun.

If REFINE #1 fails, one bounded rescue round remains.

---

Final ruling:

PR #105 APPROVED.

WRITE-WINDOW HYPOTHESIS CONFIRMED.

REFINE #1 = W1 CONFIGURATION RESCUE.

Original S6 remains FAIL until the formal rescue cells pass under unchanged criteria.
