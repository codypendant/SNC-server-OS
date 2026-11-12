# PR #112 CP-1A Profile-A duration validation: final review

Filed verbatim from cody's upload in the project thread, 2026-11-12 03:18 UTC
(relayed because the reviewer's GitHub connector is read-only).

---

PR #112 FINAL REVIEW

Repository:

"codypendant/SNC-server-OS"

PR:

"#112 — CP-1A Profile-A duration validation: PASS"

Reviewed head:

"df84f1f12b18ec0c46f6b8199a99aa1292227db2"

Decision

PR #112: APPROVED FOR MERGE AS EXPERIMENTAL EVIDENCE.

The preregistered mechanical verdict is correctly:

PASS

However:

THE CLAIM “60 s PROFILE-A WINDOW FULLY VALIDATED” IS NOT YET ACCEPTED FOR CRITERIA FREEZE.

CRITERIA: HOLD.

COMPETITOR RUNS: NOT AUTHORIZED.

REFINE #2: NOT AUTHORIZED AND NOT CONSUMED.

---

What passed

The validation executed exactly as preregistered:

- 654,569 ops/s;
- Profile A;
- 60 / 120 / 240 s;
- 3 reps each;
- 10 s warmup;
- 10 s drain;
- fresh server/data directory;
- same CP-1A binaries;
- same pilot plan;
- same manifest e;
- same PILOT-L allocation;
- same checkpoint configuration;
- same W=1;
- clean freeze gate;
- no abort;
- no rerun;
- no replacement rep.

All nine reps:

- sustained;
- zero EXEC refusals;
- zero INDEX refusals;
- zero errors;
- drained.

Results:

60 s median p99   = 1,474,560 ns
120 s median p99  = 1,146,880 ns
240 s median p99  = 1,507,328 ns

D                  = 0.05785801663786966
60 s derived bound = 1,559,875.1 ns

Therefore both 120 and 240 satisfy the preregistered numerical rule.

The PASS verdict is valid evidence and must remain recorded.

---

Finding 1 — time/order confound

Chronological p99:

1  120s-r1   1.179648 ms
2  120s-r0   1.146880 ms
3  120s-r2   1.048576 ms
4   60s-r2   1.146880 ms
5  240s-r2   1.474560 ms
6  240s-r0   1.507328 ms
7  240s-r1   1.540096 ms
8   60s-r1   1.474560 ms
9   60s-r0   1.474560 ms

There is an obvious session-position transition around runs 4–5.

The three duration classes are therefore not cleanly separated from time/session position.

In particular, two of three 60-second reference reps occur at the end of the session and raise the 60-second median to 1.474560 ms.

That raises the derived PASS bound to 1.559875 ms.

The 240-second median is then only about one histogram bucket below the limit.

The preregistration was followed correctly, but the randomized order happened to produce an experimentally weak allocation.

Do not reinterpret this as misconduct or rerun selection. It is an experimental-design finding.

---

Finding 2 — between-session p99 nonstationarity

Same rate:

"654,569 ops/s"

Same binaries.

Same plan.

Same Profile-A configuration.

Same hardware/manifest.

Same 60-second measurement window.

CP-1A

Eight relevant objective-rate observations:

- 3 sweep reps;
- 5 fixed reps.

Total p99 approximately:

"0.90–0.95 ms"

Median:

"933,888 ns"

Duration-validation session

60-second p99:

1,146,880
1,474,560
1,474,560 ns

Median:

"1,474,560 ns"

That is approximately 58% above the CP-1A median.

The sample CV of these three 60-second reps, using "pilot.cv()"'s sample-standard-deviation rule, is approximately:

"13.86%"

This crosses the Experimental Baseline's explicit warning condition for unexpected high/non-normal pilot variance.

Therefore the existing CP-1A noise input:

"5.7858%"

cannot yet be frozen as the complete campaign noise model.

---

Finding 3 — operation-class change is real

The shift is not only an aggregate histogram artifact.

Approximate mutation p99:

CP-1A objective

Deposit / transfer:

"4.8–5.5 ms"

Duration session

Deposit / transfer:

"5.6–6.6 ms"

Mutation p95 also rises materially.

The aggregate total p99 amplifies that underlying distribution shift.

---

Finding 4 — storage state is a credible hypothesis

Computed directly from "pilot.json":

CP-1A Profile A host write traffic:

1,884,936,351,744 bytes

approximately:

1.885 TB

The physical data SSD is nominally 3.84 TB.

So CP-1A generated host writes equivalent to roughly half of the drive's nominal capacity.

The duration-validation session then generated approximately another:

201 GB

Chronologically in that session:

cumulative new writes   total p99

~20 GB                  1.180 ms
~39 GB                  1.147 ms
~59 GB                  1.049 ms
~70 GB                  1.147 ms
~107 GB                 1.475 ms
~144 GB                 1.507 ms
~181 GB                 1.540 ms
~191 GB                 1.475 ms
~201 GB                 1.475 ms

This correlation does not prove causality.

Possible mechanisms include:

- SSD FTL state;
- garbage collection;
- free-block state;
- write-cache behavior;
- background device maintenance;
- filesystem/discard state;
- another persistent host/device-state variable.

Checkpoint completion itself stays approximately stable, so no single cause is established.

---

NEXT AUTHORIZED TASK

R640 storage/session-state investigation

Read-only diagnosis first.

Do not run another performance experiment yet.

Do not fstrim.

Do not blkdiscard.

Do not format the SSD.

Do not reboot merely to change state.

Do not deliberately precondition the SSD yet.

Preserve the current device state until it is characterized.

---

Capture

Create:

"bench/results/r640-storage-state-20261112/"

Capture, where available:

Device identity/state

- "lsblk -o NAME,MODEL,SERIAL,SIZE,ROTA,DISC-GRAN,DISC-MAX,DISC-ZERO"
- "lsscsi -g"
- "smartctl -x /dev/sdb"
- relevant "sg_logs" pages if supported
- "/sys/block/sdb/queue/*" relevant to scheduler, discard, write cache
- device temperature
- wear / lifetime writes / media-error information
- background scan / error counters where exposed

Do not clear any counter.

Filesystem

- "findmnt -no SOURCE,TARGET,FSTYPE,OPTIONS /srv/snc-data"
- "df -h /srv/snc-data"
- "df -i /srv/snc-data"
- "tune2fs -l" for the filesystem
- current free-space state

Discard / trim history

Capture:

- whether the device reports discard/UNMAP capability;
- whether the filesystem is mounted with continuous discard;
- "systemctl status fstrim.timer"
- "systemctl status fstrim.service"
- relevant "journalctl" history for fstrim covering the CP-1A and validation period.

We need to know whether any trim occurred between sessions.

Host state

Capture:

- uptime;
- CPU temperatures if available;
- current scaling/governor information;
- NUMA balancing;
- memory availability;
- significant kernel/storage errors since CP-1A;
- relevant "dmesg" storage events.

Read only.

---

Evidence analysis

Add a concise report that records:

1. CP-1A generated approximately 1.885 TB host writes.
2. Duration validation generated approximately 201 GB more.
3. The p99 state transition occurred after approximately 70–107 GB of additional validation-session writes.
4. Whether the device/OS exposes evidence consistent with:
   - trim state;
   - garbage collection / wear;
   - thermal state;
   - errors;
   - controller/cache changes.

Do not claim causality unless directly demonstrated.

---

Also analyze campaign methodology

Produce a proposed revision for review addressing:

A. storage preconditioning

Should every physical campaign session begin from a defined SSD state?

Possible approaches may be evaluated, but none is authorized yet.

B. between-session noise

The current N uses within-session fixed-rep CV.

That is insufficient if SNC moves materially between sessions.

Propose a deterministic SNC-only procedure that measures between-session variation before competitor data exists.

C. duration-order confounding

Future duration validation must balance duration against session position rather than relying on a shuffle that can accidentally cluster cells.

A blocked/interleaved design should be proposed.

---

No performance data in this slice

This next task is investigation + methodology design.

Do not run:

- another CP-1A;
- another duration test;
- competitor benchmarks;
- a storage preconditioning benchmark;
- REFINE #2.

Return the storage-state evidence and proposed revised method for review.

---

PR #112 merge status

PR #112 may merge.

Record both truths:

1. Preregistered duration validation verdict = PASS.
2. CRITERIA remains HOLD because the run exposed between-session nonstationarity and a time/order confound not covered by that verdict.

Do not erase the PASS.

Do not elevate the PASS into final campaign-window validation yet.

---

Final ruling:

PR #112 APPROVED FOR MERGE AS VALID EVIDENCE.

MECHANICAL DURATION VERDICT: PASS.

CRITERIA: HOLD.

NEXT: READ-ONLY STORAGE/SESSION-STATE INVESTIGATION AND METHODOLOGY REVIEW.