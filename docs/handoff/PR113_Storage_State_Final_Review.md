# PR #113 R640 storage/session-state investigation: final review

Filed verbatim from cody's upload in the project thread, 2026-11-12 03:52 UTC
(relayed because the reviewer's GitHub connector is read-only).

---

PR #113 FINAL REVIEW

Repository:

"codypendant/SNC-server-OS"

PR:

"#113 — R640 storage/session-state investigation + method proposal"

Reviewed head:

"f85e371ecb9f0dea56f17d9acd3479bad59a222d"

Decision

PR #113: APPROVED WITH CORRECTIONS.

STORAGE/SESSION-STATE EVIDENCE: ACCEPTED.

CRITERIA: HOLD.

COMPETITOR PERFORMANCE: NOT AUTHORIZED.

REFINE #2: NOT AUTHORIZED / NOT CONSUMED.

---

Required correction before merge

In "REPORT.md", replace the inaccurate heading:

«The SSD was trimmed about 3 and 6 hours before the sessions, and not since»

with:

«The SSD was trimmed once at 15:09 UTC before both sessions, and not again»

or equivalent wording.

The underlying timeline/table is accepted.

No data needs to be recollected.

---

Accepted evidence

At 2026-11-11 15:09 UTC:

"fstrim" unmapped:

"3,779,298,537,472 bytes"

from:

"/srv/snc-data /dev/sdb1"

The device supports UNMAP.

Filesystem mount:

"rw,relatime,stripe=2"

There is no continuous "discard".

Therefore deleted benchmark directories did not continuously return LBAs to the device.

No fstrim occurred between CP-1A and the duration validation.

---

SSD health

Accepted:

- SMART health OK;
- no corrected/uncorrected read errors;
- no corrected/uncorrected write errors;
- no verify errors;
- grown defects = 0;
- pending defects = 0;
- endurance used = 0%;
- current temperature 36 C;
- max since current power-on 41 C;
- SAS link counters clean;
- no data-path storage error in the kernel log.

No evidence supports a failing SSD or thermal throttling.

---

Background scan

The SSD reported:

- background medium scan active;
- progress = 79.61%;
- 12 scans over lifetime.

There is no snapshot from CP-1A or the duration session.

Therefore:

background scan interference is a plausible candidate, not a demonstrated cause.

Do not attribute the p99 shift to it.

---

Trim / FTL / GC conclusion

The evidence establishes that CP-1A and the later duration validation were not conducted from a formally controlled SSD mapping/internal-state regime.

It does not establish which internal state caused the latency movement.

GC/free-block state is not directly observable on this device.

The investigation has therefore completed its purpose.

---

A0 — AUTHORIZED NOW

Disable automatic fstrim scheduling for the duration of the controlled campaign:

sudo systemctl disable --now fstrim.timer

Then record:

systemctl is-enabled fstrim.timer
systemctl is-active fstrim.timer
systemctl status fstrim.timer --no-pager

Expected:

- disabled;
- inactive.

This prevents an uncontrolled state transition.

It does not authorize an actual trim.

Do not execute "fstrim" yet.

---

Storage start-state decision

For the first comparative campaign:

Selected baseline method

A2-a — explicit fresh-trim start state

Each formal physical session will eventually begin with:

1. explicit logged trim of "/srv/snc-data";
2. fixed 30 minute idle settle;
3. read-only A1 storage-state snapshot;
4. benchmark execution.

This method is selected for repeatability, not because it best models a permanently busy production SSD.

Claims from this campaign must therefore be scoped to that storage-start regime.

A later production/steady-state campaign may separately evaluate A2-b.

---

Important consequence

The existing CP-1A Profile-A objective cannot automatically become the final CRITERIA objective once A2-a is adopted.

CP-1A's:

- stable peak "1,090,948";
- objective rate "654,569";
- objective p99 "933,888 ns";

were measured under the previous uncontrolled storage-history regime.

The objective rate may survive revalidation, but that must be measured rather than assumed.

---

METHOD PROPOSAL B — CORRECTIONS REQUIRED

The proposed K=5 session design is directionally correct but is not approved exactly as written.

Correction B1 — distinguish method validation from final N

First validate that A2-a actually controls session-to-session movement.

Use the existing 654,569 ops/s value only as a historical anchor rate during this method-validation phase.

Do not yet call the resulting value the final campaign p99 objective.

---

Correction B2 — conservative within-session CV

Do not use:

"median(K session CVs)"

for the final noise guard.

Use:

"max(K session within-session CVs)"

for both p99 and throughput.

Final candidate noise input becomes:

N_raw =
3 × max(
    CV_p99_between,
    CV_thr_between,
    max_session_CV_p99_within,
    max_session_CV_thr_within
)

"N = max(N_raw, N_MIN)" is still set only in the later CRITERIA review.

A noisy individual session may not disappear inside a median.

---

Correction B3 — stop conditions

Before any competitor data:

STOP for methodology review if:

- "CV_p99_between > 10%"; or
- "CV_thr_between > 10%"; or
- any individual session's p99 CV > 10%; or
- any individual session's throughput CV > 10%; or
- the distribution is visibly/statefully multimodal despite CV ≤ 10%.

Do not widen N to absorb an uncontrolled state.

---

Correction B4 — symmetric sentinels

For start/end sentinel p99 values "P_start" and "P_end", test:

max(P_end / P_start, P_start / P_end) <= 1 + N

A large improvement is also nonstationarity.

Do not test only for end-of-session degradation.

---

After A2 method stability is established

Revalidate the Profile-A stable-peak boundary under the accepted storage regime.

The old boundary was:

872,759    sustained
1,090,948  sustained
1,309,138  unsustained

Preregister a balanced boundary validation around those rates.

At minimum:

- 872,759 × 3
- 1,090,948 × 3
- 1,309,138 × 3

Use a balanced/interleaved ordering.

If the old bracket reproduces

If:

- 872,759 sustains 3/3;
- 1,090,948 sustains 3/3;
- 1,309,138 is unsustained under the frozen sustained rule;

then the old stable-peak boundary is validated under A2-a.

The objective rate may remain:

"654,569 ops/s"

because it is still the largest original grid point ≤ 0.70 × S.

If the bracket does not reproduce

STOP.

If 1,309,138 becomes sustained or 1,090,948 becomes unsustained, the stable peak has moved.

A new Profile-A sweep is then required before CRITERIA.

Do not extrapolate S.

---

Final objective p99

If the stable-peak boundary remains unchanged, derive the campaign p99 objective from the controlled A2-a sessions, not from the old single CP-1A session.

Use the preregistered aggregate of the controlled SNC-only objective-rate sessions.

Recommended:

median of the K session medians

at the validated objective rate.

This value is fixed before competitor performance is observed.

---

Duration validation

The Latin-square/block concept in METHOD_PROPOSAL C is accepted in principle.

Use only after:

1. A2-a is shown reproducible;
2. stable-peak/objective rate is revalidated;
3. final SNC-only noise input is available.

Then use:

Block 1: 60, 120, 240
Block 2: 120, 240, 60
Block 3: 240, 60, 120

with start/end stationarity sentinels.

Judge long-window p99 by paired within-block ratios.

Use the accepted campaign noise guard, not the old CP-1A-only "D=0.057858".

---

A1 session snapshots

Approved.

Every controlled physical session should capture at start and end:

- "/proc/diskstats";
- SMART/SCSI health and temperature;
- background-scan status/progress;
- most recent trim;
- filesystem lifetime writes;
- uptime / boot ID.

Do not clear counters.

A background scan being present is recorded explicitly.

If later SNC-only method-validation sessions show scan state produces uncontrolled >10% movement, STOP and revise the method rather than hiding it in N.

---

NEXT AUTHORIZED REPOSITORY TASK

Update "METHOD_PROPOSAL.md" with the corrections above.

Create a concrete preregistration/driver/test package for:

A2-a storage-state reproducibility validation

but do not run it yet.

The preregistration should specify:

- K=5 sessions;
- explicit trim + 30 minute settle before each;
- A1 start/end snapshots;
- historical anchor rate 654,569;
- five open-loop fixed reps per session;
- five closed-loop capacity reps per session;
- exact session/run order;
- maximum-within-session CV rule;
- between-session CV rule;
- «10% stop rules;»
- symmetric sentinel rule;
- no competitor;
- no threshold adjustment after data.

Return that preregistration for independent review before the first explicit trim/performance session.

---

Still unauthorized

- executing A2-a performance sessions;
- stable-peak boundary revalidation;
- duration Latin-square run;
- CRITERIA freeze;
- competitor performance;
- REFINE #2;
- product optimization.

---

Final ruling:

PR #113 EVIDENCE APPROVED.

MERGE AFTER THE ONE TIMELINE-WORDING CORRECTION.

DISABLE THE AUTOMATIC FSTRIM TIMER.

NEXT: PREREGISTER THE CONTROLLED STORAGE-STATE REPRODUCIBILITY EXPERIMENT — DO NOT RUN IT YET.