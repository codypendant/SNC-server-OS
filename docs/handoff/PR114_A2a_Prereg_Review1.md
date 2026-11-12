# PR #114 A2-a preregistration: review 1 (CHANGES REQUIRED)

Filed verbatim from cody's upload in the project thread, 2026-11-12 04:21 UTC
(relayed because the reviewer's GitHub connector is read-only).

---

PR #114 REVIEW

Repository:

"codypendant/SNC-server-OS"

PR:

"#114 — A0 done + A2-a storage-state reproducibility preregistration"

Reviewed head:

"5845c38d447ed8709ec7c9514c1a96b707d8d04f"

Decision

CHANGES REQUIRED.

DO NOT RUN THE A2-a REPRODUCIBILITY EXPERIMENT YET.

The overall architecture is accepted:

- A0 completed correctly;
- A2-a remains the selected storage-start method;
- K=5 remains;
- historical anchor 654,569 remains;
- five fixed + five capacity reps per session remain;
- start/end sentinels remain;
- explicit trim + 30-minute settle remains;
- A1 snapshots remain;
- no competitor;
- no CRITERIA;
- no REFINE #2.

The corrections below must be made and returned for review before the first explicit trim.

---

1. Fix the circular sentinel rule

Current rule:

"sentinel ratio <= 1 + N_raw"

is not accepted.

"N_raw" is calculated from the same five-session experiment. Therefore noisy data can enlarge the threshold used to decide whether that data is stationary.

At the maximum permitted CV below the 10% STOP line:

"N_raw ≈ 3 × 10% = 30%"

That could permit nearly 30% start-to-end p99 movement while declaring a session stationary.

That is too permissive and circular.

Required rule

For this A2-a method-validation experiment, freeze:

"SENTINEL_LIMIT = 0.10"

For every session:

max(P_end / P_start, P_start / P_end) <= 1.10

Equality passes.

Any symmetric movement strictly greater than 10%:

STOP

This rule is independent of the measured N_raw.

N_raw remains a result of the experiment and may later inform CRITERIA.

Update:

- "PREREG.md"
- "METHOD_PROPOSAL.md"
- "a2a.py"
- "test_a2a.py"
- mutation suite.

Add mutants proving:

- 1.10 passes;
- «1.10 fails;»
- both degradation and improvement are symmetric;
- replacing fixed 10% with "N_raw" is caught.

---

2. Add a per-session environment guard

The existing full CP-1A preflight at invocation start is good but insufficient for five storage-reset sessions spanning roughly 4+ hours.

Before each session's trim, perform a lightweight fail-closed guard.

At minimum verify:

- same boot ID as invocation start;
- git HEAD unchanged;
- working tree still clean;
- "fstrim.timer" remains "disabled";
- "fstrim.timer" remains "inactive";
- "kernel.numa_balancing == 0";
- every CPU governor remains "performance";
- "/srv/snc-data" still resolves to "/dev/sdb1";
- physical data disk remains "sdb";
- mount options remain the accepted ext4 configuration;
- plan SHA remains frozen;
- manifest SHA remains frozen;
- "snc_server" SHA remains frozen;
- "net_bench" SHA remains frozen.

Do not rebuild binaries between sessions.

This is an identity/state guard, not another full pilot initialization.

If any check fails:

INCOMPLETE — STOP.

No replacement session.

Record the per-session guard result.

---

3. Prove each explicit trim actually occurred

A successful "fstrim" process exit alone is not enough evidence for the defined start-state procedure.

For each session record:

- "sdb" discard-sector counter immediately before trim;
- fstrim stdout;
- fstrim return code;
- discard-sector counter immediately after trim;
- parsed bytes reported by fstrim.

Require:

- return code = 0;
- output identifies "/srv/snc-data";
- reported trimmed bytes > 0;
- "sdb" discard-sector delta > 0.

If any fails:

INCOMPLETE — STOP.

Do not continue to the 30-minute settle.

This ensures the experiment really executed the state-reset operation it claims.

---

4. Make the 30-minute settle a controlled idle period

Current driver records diskstats during the settle but does not judge them.

The point of the settle is to reach the benchmark start without host writes/discards to the data SSD.

Freeze this rule:

During the 1,800-second settle:

- data-disk written-sector delta must be 0;
- data-disk discard-sector delta must be 0.

If either is nonzero:

INCOMPLETE — STOP FOR REVIEW.

Do not silently accept background writes to "sdb".

Activity on the boot disk may be recorded but is not automatically invalid.

If testing shows the mounted ext4 filesystem performs unavoidable data-disk maintenance during an otherwise idle settle, return that evidence for review rather than inventing a tolerance after data.

---

5. Strengthen capacity-rep validity

Current capacity check accepts a rep unless:

- "failed"; or
- "errors != 0".

That is too narrow.

For every capacity rep require at least:

- "returncode == 0";
- not "failed";
- "errors == 0";
- "stale == 0";
- "index_errors == 0".

EXEC overload/refusal remains expected in closed-loop capacity measurement and is not itself a failure.

Do not change the throughput calculation.

Add deterministic negative tests for:

- nonzero client return code;
- stale response;
- index error.

---

6. Fail closed on unusable A1 state capture

A1 is now part of the experimental method.

Do not continue a session if the state snapshot is materially incomplete.

At minimum require each start/end snapshot to successfully provide:

- diskstats;
- boot ID;
- uptime;
- SMART health;
- temperature;
- background-scan status/progress if the device exposes it;
- endurance value;
- defect/error fields;
- "sg_logs" output;
- filesystem lifetime-write information.

"smartctl -x" on this SAS device may return a nonzero bitmask while still providing the required data, so do not blindly require "smartctl rc == 0".

Instead validate the required parsed fields/output.

For commands such as "sg_logs" and "tune2fs" that normally return cleanly, record and validate their success.

If the required A1 evidence cannot be captured:

INCOMPLETE — STOP.

---

7. Multimodality rule

The deterministic gap rule may remain:

"adjacent session-median gap > 2 × widest within-session range"

as an automatic STOP signal.

But explicitly state that it is supplementary, not a proof that a distribution is unimodal when it does not fire.

The final independent result review may still reject a visibly/statefully clustered result even if:

- every CV ≤10%; and
- the automatic gap rule does not trigger.

Do not change the rule after seeing results.

---

8. Session order

The current order is accepted:

S0
F1 C1
F2 C2
F3 C3
F4 C4
F5 C5
S1

Using the same order in every session is intentional here because this experiment is testing reproducibility of a defined session procedure.

No randomization is required for B.

The later duration experiment remains the Latin-square design.

---

9. A0 evidence

A0 is accepted.

Verified state:

- fstrim timer was enabled/active before;
- "systemctl disable --now fstrim.timer" succeeded;
- timer became disabled/inactive;
- "fstrim.service" inactive;
- no additional trim occurred;
- no continuous-discard mount option.

Do not re-enable automatic fstrim during the controlled campaign.

---

10. Sudoers

The narrow sudoers design is accepted in principle:

- exact "fstrim" command;
- exact "smartctl";
- exact "sg_logs";
- exact "tune2fs".

Install only after this corrected preregistration receives final approval.

Record:

"visudo -c"

result.

Do not grant generic root shells or wildcarded disk commands.

---

Required test/mutation additions

The revised tests must explicitly cover:

- fixed 10% sentinel limit;
- N_raw cannot influence sentinel acceptance;
- symmetric sentinel improvement/degradation;
- per-session boot-ID drift;
- per-session governor drift;
- per-session NUMA drift;
- dirty Git tree / HEAD drift;
- timer re-enabled between sessions;
- mount/data-device drift;
- fstrim rc failure;
- fstrim reports zero bytes;
- discard-sector delta zero;
- data-disk write during settle;
- data-disk discard during settle;
- capacity returncode nonzero;
- capacity stale;
- capacity index_errors;
- incomplete A1 capture.

Keep the existing useful mutation coverage.

---

Verdict definitions after correction

STABLE

Only if:

- all five sessions complete;
- all 60 benchmark runs exist;
- all anchor/fixed/sentinel reps sustain;
- capacity reps meet their validity conditions;
- every explicit trim is proven;
- every settle is clean;
- every A1 capture is usable;
- all per-session environment guards pass;
- all between/within CVs ≤10%;
- no automatic multimodal condition fires;
- every symmetric sentinel ratio ≤1.10.

Then:

A2-a storage-start method is reproducible enough to proceed to the separately reviewed stable-peak boundary validation.

This does not yet freeze CRITERIA.

STOP

A completed experiment violates a statistical/stationarity rule.

Return for methodology review.

Do not widen thresholds or replace sessions.

INCOMPLETE

Environment drift, trim failure, settle contamination, missing state evidence, aborted process, missing run, or other execution-integrity failure.

Stop immediately.

Do not replace the session.

---

Still unauthorized

- installing/running A2-a before corrected review;
- stable-peak boundary validation;
- duration Latin-square validation;
- CRITERIA freeze;
- competitor performance;
- REFINE #2.

---

Final ruling:

PR #114 IS NOT YET AUTHORIZED TO RUN.

APPLY THE CORRECTIONS ABOVE AND RETURN THE UPDATED PR FOR REVIEW.