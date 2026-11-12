# PR #111 CP-1A R640 result: final review

Filed verbatim from cody's upload in the project thread, 2026-11-12 01:52 UTC
(relayed because the reviewer's GitHub connector is read-only).

---

PR #111 FINAL REVIEW

Repository:

"codypendant/SNC-server-OS"

PR:

"#111 — CP-1A R640 result: complete, no abort"

Reviewed head:

"34790ee25bb096c7ffb32cec15ede15304521f72"

Decision

PR #111: APPROVED FOR MERGE.

CP-1A EXECUTION EVIDENCE: VALID.

CP-1A COMPLETE.

CRITERIA FREEZE: HOLD pending one narrow Profile-A duration validation.

REFINE ROUND 2 IS NOT AUTHORIZED AND IS NOT CONSUMED.

No competitor performance runs.

---

Execution integrity

Verified:

- run HEAD = PR #110 merge
  "b2948a31dc35aeb1af61b66ae49a93d3d96462a8"
- no intervening commit;
- freeze gate passed;
- clean tree;
- plan SHA matched;
- manifest-e SHA matched;
- "snc_server" and "net_bench" hashes matched;
- governors = performance;
- NUMA balancing = 0;
- PILOT-L allocation;
- scratch/output isolated from data SSD;
- 186 expected runs completed;
- 93 Profile A;
- 93 Profile C;
- no abort;
- no failed or unparsable run.

The result set is admissible evidence.

---

Accepted CP-1A results

Profile A — LOCAL_DURABLE

Calibration P0:

"4,363,795 ops/s"

Stable peak S:

"1,090,948 ops/s"

Objective rate:

"654,569 ops/s"

Objective p99:

"933,888 ns"

Fixed objective reps:

"5 / 5 sustained"

CV_p99:

"0.019286"

Capacity median:

"4,031,898 ops/s"

CV_throughput:

"0.003462"

Pilot noise input:

"3 × max(CV_thr, CV_p99) = 0.057858"

Memory:

approximately "515.5 B/object"

Stable-peak boundary

Next grid point:

"1,309,138 ops/s"

Three reps:

- rep: 0 refusals
- rep: 33 EXEC refusals
- rep: 0 refusals

No client-limited-suspect condition.

Under the frozen rule requiring all three reps to sustain, the point correctly fails.

Therefore:

"S = 1,090,948"

is accepted.

---

Profile C — MEMORY

Calibration P0:

"6,079,563 ops/s"

Stable peak:

"5,775,584 ops/s"

Objective rate:

"3,951,715 ops/s"

Objective p99:

"524,288 ns"

Fixed reps:

"5 / 5 sustained"

CV_p99:

"0"

Capacity median:

"6,030,447 ops/s"

CV_throughput:

"0.004240"

Noise input:

"0.012719"

Memory:

approximately "515.3 B/object"

Profile C duration validation shows no drift.

Accepted.

---

Profile-A duration finding

The original duration check runs at:

"2,181,897 ops/s"

because the preregistered algorithm chose the largest grid rate ≤ P0/2.

But after the sweep:

"S = 1,090,948"

Therefore the duration-check rate is approximately:

"2.0 × S"

and is already outside the measured sustainable region.

Observed:

Window| Periodic checkpoints| Sustained| Refusals| p99
30 s| 0| yes| 0| 1.03 ms
60 s| 1| no| 6,900| 2.10 ms
120 s| 2| no| 15,328 EXEC + 4,452 INDEX| 2.23 ms
240 s| 4| no| 48,381 EXEC + 31,497 INDEX| 2.29 ms

Interpretation:

This does not establish that the 60-second sweep window is too short.

It establishes that the duration-test rate selection is unsuitable once P0/2 lies above the stable peak.

The 30-second result also omits checkpoint exposure entirely, while the longer runs contain repeated checkpoints.

---

Evidence supporting the existing 60 s operating window

The five independently planned Profile-A fixed reps at the actual objective rate:

"654,569 ops/s"

all contain a checkpoint.

Results:

- 5/5 sustained;
- zero EXEC refusals;
- zero INDEX refusals;
- zero errors/stale/index errors;
- drained;
- p99 = approximately 0.90–0.95 ms;
- p99.9 = approximately 8.4–8.9 ms;
- CV_p99 = 1.93%.

Thus the CP-1A sweep, objective and noise measurements are not rejected.

---

NEXT AUTHORIZED TASK

Profile-A objective-rate duration validation

This is a pilot-method validation, not REFINE round 2.

No product code change.

No tuning.

No threshold change.

No competitor.

Use the exact accepted Profile-A configuration and binaries.

Rate

Exactly the already-derived CP-1A objective:

"654,569 ops/s"

This rate is selected by the preregistered CP-1A algorithm and is not chosen post hoc for favorable performance.

Cells

Run:

- 60 s × 3 reps
- 120 s × 3 reps
- 240 s × 3 reps

Total:

9 runs

Use the normal 10 s warm-up and 10 s drain.

Fresh server/data directory for every rep.

Use the same:

- 5M objects;
- workload mix;
- 8 shards;
- 8 client threads;
- PILOT-L CPUs;
- manifest e;
- scratch isolation;
- checkpoint interval;
- pacing;
- W=1;
- target/max-age;
- dsync mode;
- sync-token setting.

---

Preregister interpretation before data

All nine runs must satisfy the existing "pilot.sustained()" rules.

In particular:

- zero errors;
- zero refusals;
- completed ≥ 99% of owed work;
- end backlog ≤ 1%;
- drained.

Latency-drift reference

Use the same already-approved noise construction:

"D = 3 × CV_p99_fixed_A"

From CP-1A:

"D = 0.0578580166"

For the new characterization, compute the median p99 of the three 60 s reps.

The median p99 of the 120 s and 240 s cells must each be no more than:

"60s median p99 × (1 + D)"

Do not alter D after observing the new runs.

p99.9 and max are report-only diagnostics.

---

Decision after duration validation

PASS

If:

- all 9 reps sustain; and
- 120 s median p99 is within D of 60 s; and
- 240 s median p99 is within D of 60 s;

then:

60 s Profile-A measurement window validated.

The existing CP-1A sweep/objective/fixed/capacity results stand.

Next step becomes formal "CRITERIA.md" review.

No full CP-1A rerun.

FAIL

If any duration cell fails sustained or exceeds the predeclared p99 drift bound:

STOP.

Do not select a longer window after seeing the result.

Return for review.

A revised measurement window would then require a methodology revision and affected Profile-A pilot rerun before CRITERIA.

This is still not automatically REFINE round 2.

---

Evidence handling

Preregister the characterization before any new duration data.

Preserve all raw client/server logs.

Commit result evidence.

Return it for independent review.

---

PR #111 merge

PR #111 contains valid CP-1A evidence and may merge now.

Record the original duration anomaly exactly as observed.

Do not erase or reinterpret it as a pass.

---

Still unauthorized

Until duration validation review:

- CRITERIA freeze
- competitor performance runs
- REFINE #2
- S5
- B2
- adaptive window controller
- global W default change
- Agent-SNC
- Rust
- DPDK/VPP

Final ruling:

PR #111 APPROVED FOR MERGE.

CP-1A evidence accepted.

One narrow duration-method validation remains before CRITERIA.