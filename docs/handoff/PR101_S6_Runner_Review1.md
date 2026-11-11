# PR #101 S6 runner + final preregistration: independent review 1

Filed verbatim from cody's message in the project thread, 2026-11-11 17:59 UTC
(relayed because the reviewer's GitHub connector is read-only).

Reviewed head: `e34cfe03a132d1151eaa85e807c371f3fe332d52`

---

PR #101 INDEPENDENT REVIEW — REVIEW 1

Decision

PR #101: CHANGES REQUIRED.

Runner architecture: substantially approved.

S6 preregistration: substantially approved.

S6: NOT GO.

Do not merge PR #101 yet.

Fix the four closure items below on the same PR, rerun the relevant validation and mutations, and return the new head for independent review.

---

What is already approved

1. Scope

The PR stays within the authorized harness/preregistration slice.

Verified:

- no C++ product change;
- runner is Python;
- rules/tests are Python;
- preregistration/docs only;
- no actual S6 cell has run.

The current binaries remain from post-PR99 main.

That is correct.

---

2. Binary/source lineage

Accepted.

Current C++ source identity:

"91278d7fbc5dc93dc81ceb317a0626f308ec846e"

PR #100 was documentation-only.

PR #101 changes no C++.

Frozen binary identities are therefore valid candidates:

- current "snc_server":
  "69a51b695e2029942039d386abaa9755616f31327ff63e0420dd0c5c8ba25ee1"

- common current "net_bench":
  "9fc5494404747f8242680552588bd6f986e3e00b265e45ee67262c513a93c581"

- "ckpt_walk_cost":
  "4707dac135a6fc72c41a712075678aa24ccbf7ad74d546962623026584b5d2d2"

- pre-S1 server:
  "78f471efb7e7951bf8cf14637e03120d6460dc989317c5c32fb073af23806ff7"

Pre-S1 source commit remains:

"389b6234855082a4591c6677d0a303c3add3319d"

The one-common-"net_bench" rule is implemented correctly.

---

3. Compatibility check

Accepted.

The current "net_bench" successfully drove the "389b623" server using the runner's own process path.

The fact that this was performed on the R640 rather than a dev VM is not a blocker because it was deliberately made non-campaign:

- 5-second closed-loop window;
- 8-second checkpoint interval;
- boot-disk data;
- no S6 rates;
- no S6 verdict;
- throughput and latency explicitly not interpreted.

It proved interface/harness compatibility only.

No dev-VM repeat is required solely because of host location.

Do not reuse its performance numbers as S6 evidence.

---

4. Freeze mechanism

Direction approved.

A real run correctly requires:

- "runner_commit";
- a filed "go_record";
- exact binary hashes;
- clean tree;
- runner commit as ancestor;
- no unauthorized code changes after runner merge;
- machine identity;
- disk isolation.

The intended sequence remains:

PR #101 approved and merged
→ docs-only freeze/GO commit
→ runner refuses unless freeze and GO are present.

Keep this mechanism.

---

5. S6 cells and fixed methodology

Approved.

Keep:

- rep-major order:
  "N, P, H, C, C0"
- 3 reps each;
- P: 436,646 open-loop, checkpoint;
- N: 436,646 open-loop, no checkpoint;
- H: 2,183,233 open-loop, checkpoint;
- C: current closed-loop checkpoint capacity;
- C0: pre-S1 closed-loop checkpoint capacity.

Keep:

- same current "net_bench" for C and C0;
- same-SHA N median as p99 reference;
- 20 / 65 / 155 / 5 / batch-16 pacing;
- target 30 s;
- max age 60 s;
- 100 ms observation cadence;
- exact PASS/FAIL thresholds already preregistered.

Do not tune these from later S6 data.

---

Required closure item 1 — measured client exit status

Finding

"Driver.execute()" records:

"r["client_rc"] = run.returncode"

but "correctness_ok()" never checks it.

There is therefore no explicit fail-closed rule saying that the measured "net_bench" process itself must exit successfully.

Today normal "net_bench" workload errors also tend to produce counters that fail rule 7, so the omission is partly redundant under ordinary paths.

That is not sufficient for an experimental gate.

A truncated, abnormal, future, or otherwise non-standard client failure must never be able to pass simply because enough output parsed.

Required fix

In "correctness_ok()" require:

"client_rc == 0"

for both open-loop and closed-loop measured runs.

Missing "client_rc" must also fail.

This is a result failure, not an automatic rerun.

Do not turn a genuine measured-client failure into a harness VOID unless it happened before the measured process actually ran.

Required tests

Add deterministic tests showing:

- "client_rc = 0" passes when everything else passes;
- "client_rc = 1" fails rule 7;
- negative/signal-style rc fails;
- missing rc fails;
- a P rep with nonzero client rc produces FAIL, not rerun;
- a C or C0 rep with nonzero client rc fails the capacity guard.

Add a negative mutation that removes the client-rc check and prove the test catches it.

---

Required closure item 2 — sampler must not catch up with bursts

Finding

Current sampler scheduling is approximately:

```
nxt += period
stop.wait(max(0, nxt - now))
```

If GAUGES/HEALTH takes substantially longer than 100 ms, "nxt" falls behind real time.

The next loop then waits zero.

If the delay was large enough, several subsequent loops can also wait zero.

That produces a burst of back-to-back admin requests while trying to "catch up" missed sampling deadlines.

This is exactly the wrong behavior during pressure: a delayed observer must not create additional observer load.

The preregistration already defines long intervals as gaps. Use that model consistently.

Required behavior

A missed sampling period is a gap, not work owed by the sampler.

After each sampling cycle:

- advance to the next future sampling deadline;
- skip every deadline already missed;
- never issue multiple immediate cycles to make up missed samples.

Acceptable implementation shapes include:

```
nxt += period
now = time.monotonic_ns()
if nxt <= now:
    missed = (now - nxt) // period + 1
    nxt += missed * period
```

or another equivalent no-burst rule.

Do not alter the nominal 100 ms cadence.

Continue reporting the resulting gap from timestamps.

Required tests

Factor deadline advancement into a deterministic helper or otherwise test it without timing flakiness.

Test:

- normal 100 ms progression;
- a 50 ms request does not skip unnecessarily;
- a 250 ms overrun skips missed deadlines;
- a 1 second overrun does not generate ten immediate follow-up samples;
- timestamp gap reporting remains correct.

Add a negative mutation restoring catch-up behavior and catch it.

---

Required closure item 3 — C and C0 must actually contain a checkpoint

Finding

P and H explicitly verify that a checkpoint began inside their measured window.

C and C0 do not.

But the preregistered capacity guard is:

closed-loop checkpoint capacity

for current versus pre-S1.

A 60-second server checkpoint interval normally lands inside the 60-second closed-loop run because the 5M-object load has so far taken far less than 60 seconds.

That is an expectation, not a proof.

If loading, startup, or environment timing changes enough that the checkpoint occurs before the measured closed-loop capacity window, the runner could compare ordinary no-checkpoint capacity while labelling it checkpoint capacity.

That must fail closed.

Required fix

For C and C0, bound a checkpoint begin from "ckpt_seq" samples just as the runner already does for P/H.

Require at least one checkpoint begin to be bounded inside the closed-loop measured interval.

For closed-loop cells the measured interval can be conservatively represented by the runner's measured-process start/end times.

Use the same "both bounds inside" rule.

If no checkpoint is proven inside the measured C/C0 window:

classify that rep as:

"void = "window""

and apply the existing one-rerun-only policy.

A second checkpoint-window VOID retains the existing session-VOID behavior.

Do not silently use a C/C0 capacity rep without a checkpoint.

Required tests

Add:

- C checkpoint inside window → accepted;
- C0 checkpoint inside window → accepted;
- C outside window → one rerun;
- C0 outside window → one rerun;
- second window failure → session VOID;
- a capacity rep with no observable checkpoint begin is not accepted.

Add a mutation that removes the C/C0 window check and catch it.

---

Required closure item 4 — tighten storage machine identity

S6 is specifically a checkpoint/storage regression.

Current preflight verifies:

- SSD model;
- filesystem UUID;
- ext4;
- mount prefix.

That is not enough to freeze the storage state that manifest e recorded.

Add checks for the data SSD state used by the baseline:

- scheduler contains active "[mq-deadline]";
- write cache is "write back";
- FUA capability/state matches the recorded manifest ("fua=1") if exposed by the running kernel's sysfs interface.

Also tighten the filesystem option check so it confirms the expected ext4 configuration rather than merely accepting any option string beginning with "rw,relatime".

Manifest e recorded:

"rw,relatime,stripe=2"

Use a robust set/containment check suitable for the actual kernel representation.

Do not hard-code device letter "sdb"; resolve the data disk through the existing isolation/device resolver and inspect the resolved device.

Required tests

Machine-identity unit/fake tests should establish:

- wrong scheduler fails;
- wrong write-cache mode fails;
- wrong filesystem UUID fails;
- wrong/missing required mount option fails;
- correct manifest-e storage state passes.

If FUA is not exposed by this kernel through a stable sysfs field, document that explicitly rather than inventing a check.

---

Existing test/evidence status

The existing test design is otherwise strong.

Accepted:

- exact p99 3/2 boundary;
- lateness 0.1% boundary;
- p99.9 10 ms boundary;
- backlog 1024 boundary;
- capacity 95% boundary;
- refusal rules;
- correctness/drained rules;
- medians;
- VOID/rerun policy;
- timestamp-based time-above calculations;
- absent old-server gauges represented as absent rather than zero;
- one common "net_bench";
- freeze refusal;
- 19/19 existing mutations caught.

Keep all of them.

Do not discard the first stale-bytecode mutation attempt; keep the disclosure that it was invalid and rerun correctly with bytecode caching disabled.

---

Validation required after fixes

Run at minimum:

1. "tests/test_s6.py"
2. "test_pilot"
3. "test_profile_check"
4. "test_ckpt_cli"

Run the expanded negative mutation suite and require all mutations caught.

Repeat:

- dry-run preflight;
- real-run-before-freeze refusal.

No S6 cell.

The compatibility check does not need to be rerun unless the fixes change command construction or old/current server interaction.

The fixes above should not.

---

Return for review

Return PR #101 with:

- new head SHA;
- exact changes for items 1-4;
- updated tests;
- mutation count/result;
- updated preregistration text;
- dry-run result;
- pre-freeze refusal result.

Do not merge until the independent re-review.

Do not create the GO/freeze record yet.

---

Authorization boundary

Still forbidden:

- actual S6 run;
- CP-1A;
- comparator systems;
- CRITERIA freeze;
- S5;
- B2;
- Agent-SNC;
- Rust;
- DPDK/VPP.

Final ruling:

PR #101: CHANGES REQUIRED.

S6: NOT GO.

Standing rules:

Always verify, never assume.

Fix confirmed defects on site.

A monitoring harness must not become load when the system is already late.

A checkpoint-capacity cell must prove that it actually measured a checkpoint.

Any nonzero measured-client exit fails closed.
