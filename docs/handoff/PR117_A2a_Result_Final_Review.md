# PR #117 A2-a storage-state reproducibility result: final review (STOP accepted)

Filed verbatim from cody's upload in the project thread, 2026-11-12 22:08 UTC
(relayed because the reviewer's GitHub connector is read-only).

---

PR #117 FINAL REVIEW

Repository:

"codypendant/SNC-server-OS"

PR:

"#117 — A2-a storage-state reproducibility result: STOP"

Reviewed head:

"336c9eadc8b38d7fe6feeb2170eea6ff3cb781eb"

Decision

PR #117: APPROVED FOR MERGE AS VALID EXPERIMENTAL EVIDENCE.

PREREGISTERED VERDICT: STOP — ACCEPTED.

A2-a HAS NOT YET BEEN PROVEN REPRODUCIBLE FOR SATURATION THROUGHPUT.

STABLE-PEAK BRACKET: HOLD.

CRITERIA: HOLD.

COMPETITORS: NOT AUTHORIZED.

REFINE #2: NOT CONSUMED.

---

Execution validity

The experiment itself is valid.

Verified:

- K = 5 complete sessions;
- 60 / 60 runs;
- no replacement session;
- no rerun;
- no threshold change;
- all environment guards pass;
- correct boot and HEAD throughout;
- approved Samsung SSD throughout;
- persistent UUID / model / serial / WWN identity throughout;
- "fstrim.timer" disabled/inactive;
- "fstrim.service" inactive before each session;
- NUMA balancing 0;
- all governors performance;
- all frozen hashes unchanged;
- every pre/post-trim sync succeeds;
- all five trims proven;
- every 30-minute settle has:
  - 0 written sectors;
  - 0 discarded sectors;
- all A1 captures valid.

No evidence is discarded.

---

Statistical result

Session p99 medians:

k1  966,656 ns
k2  983,040 ns
k3  999,424 ns
k4  966,656 ns
k5  983,040 ns

Between-session p99 CV:

"1.40%"

Excellent stability.

Capacity medians:

k1  4,021,146
k2  4,356,828
k3  4,010,715
k4  4,033,870
k5  3,996,693 ops/s

Between-session capacity CV:

"3.75%"

All ordinary CV rules pass.

However, the supplementary preregistered multimodality rule correctly finds:

session-median gap:
322,958 ops/s

2 × widest within-session range:
217,040 ops/s

Therefore:

"322,958 > 217,040"

and the experiment's frozen verdict is:

STOP

That verdict stands.

---

Session 2 is a genuine separated state

The four ordinary sessions cluster approximately:

"3.997–4.034 M ops/s"

Session 2:

"4.357 M ops/s"

approximately 8% above the normal cluster.

Its five capacity reps are individually:

4,331,074
4,357,874
4,351,347
4,356,828
4,360,923

This is not one lucky outlier.

The entire session is in the faster state.

---

Capacity decomposition

Approximate median capacity state:

Sessions 1 / 3 / 4 / 5

throughput      ~4.0 M ops/s
total p99       ~11.5–12.1 ms
CPU/op          ~1.83–1.84 us
write bytes/op  ~69–70 B

Session 2

throughput      4.357 M ops/s
total p99       ~9.7 ms
CPU/op          ~1.70 us
write bytes/op  ~71 B

The approximately 7–8% reduction in CPU time/op closely accompanies the approximately 8% throughput gain.

---

Not a simple whole-session CPU-speed effect

At the fixed 654,569 ops/s anchor, median CPU/op remains essentially the same:

k1  ~10.168 us/op
k2  ~10.207 us/op
k3  ~10.206 us/op
k4  ~10.167 us/op
k5  ~10.215 us/op

So session 2 only enters the lower-cost state under saturation.

This is consistent with a saturation/queueing/batching interaction rather than merely a globally faster CPU for that entire session.

---

Checkpoint behavior

Session-2 capacity checkpoint times are approximately:

4.79–5.01 s

A normal session-3 comparison:

4.82–5.19 s

No major checkpoint-duration change explains the throughput difference.

---

Captured SSD state

Session 2 does not obviously differ from the other sessions in:

- trim success;
- trim volume;
- clean settle;
- SSD temperature;
- background medium-scan state;
- SMART health;
- device identity;
- error counters.

Do not attribute the state to SSD GC, background scan or trim without additional evidence.

---

NEXT AUTHORIZED TASK — READ-ONLY SESSION-2 POSTMORTEM

Do not run another performance experiment yet.

Use existing host telemetry/logs from:

2026-11-12 16:04–19:57 UTC

to determine whether session 2 corresponds to a host-level state difference.

Create:

"bench/results/a2a-session2-postmortem-20261112/"

1. Sysstat / sar

If available, preserve the raw sysstat file covering November 12 and extract the experiment window.

At minimum investigate:

CPU

- aggregate and per-CPU utilization;
- frequency / MHz if sysstat recorded it;
- run queue / load;
- context switches;
- interrupts;
- steal/iowait if applicable.

Relevant commands may include, as supported by the installed sysstat version:

sar -u
sar -P ALL
sar -q
sar -w
sar -m FREQ

using the archived November-12 sysstat file and exact experiment time range.

Do not invent unavailable metrics.

2. Block-device telemetry

Use historical sysstat data for the approved Samsung SSD/current kernel device during each session.

Compare:

- tps;
- reads/writes per second;
- MB/s;
- await;
- queue size;
- utilization;
- discard activity if recorded.

Relevant:

sar -d

or equivalent from the archived sysstat data.

Compare session 2 against sessions 1, 3, 4 and 5.

3. Thermal / frequency evidence

Search existing kernel/journal logs for the experiment interval for:

- thermal throttling;
- CPU/package throttling;
- MCE;
- frequency/power-limit events;
- storage reset/timeout/errors.

Do not run a new benchmark.

Current counters may be captured read-only but must be labelled current, not historical.

4. Services/process activity

Use the journal/sysstat/process-accounting evidence that exists to determine whether a background service or scheduled job uniquely overlapped session 2.

The existing A1 "units_started" list is useful but not sufficient if sysstat/journal can provide more.

5. Existing benchmark telemetry

Produce one consolidated table per session containing at least:

- p99 anchor median;
- capacity median;
- capacity CPU us/op;
- capacity write bytes/op;
- capacity p99;
- checkpoint duration range;
- trim bytes;
- SSD temperature;
- background scan start/end;
- any historical CPU-frequency metric found;
- any historical disk-await/util metric found.

---

Decision after the read-only postmortem

Do not automatically rerun anything.

Return the evidence for review.

Possible outcomes:

A. A concrete host-state difference is found

Then propose the smallest deterministic guard/control needed to eliminate it.

Do not implement or rerun without review.

B. No observable cause is found

Then return that fact.

We will decide whether to:

- strengthen the start-state procedure;
- instrument CPU frequency/thermal/storage state prospectively;
- or design one bounded causal characterization.

Do not retroactively declare the current STOP a PASS.

---

PR #117 merge

Merge it.

Preserve:

A2-a result = STOP

and the complete evidence exactly as observed.

The experiment was valuable: it successfully falsified the assumption that trim + 30-minute idle settle alone guarantees one saturation-capacity state on this host.

---

Still unauthorized

- stable-peak bracket;
- new sweep;
- duration Latin square;
- CRITERIA;
- competitors;
- REFINE #2;
- product tuning;
- another A2-a session.

Final ruling:

PR #117 APPROVED FOR MERGE.

STOP ACCEPTED.

NEXT: READ-ONLY SESSION-2 POSTMORTEM USING EXISTING HOST TELEMETRY.