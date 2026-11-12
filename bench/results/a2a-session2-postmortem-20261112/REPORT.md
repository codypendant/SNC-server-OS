# A2-a session-2 postmortem (read-only; for review)

Task: `docs/handoff/PR117_A2a_Result_Final_Review.md`, NEXT AUTHORIZED TASK. No benchmark was run, nothing was
rerun, the PR #117 STOP is unchanged. Everything here is computed from telemetry that already existed; tables are
in `tables.txt`, numbers in `postmortem.json`, code in `postmortem.py` and `events.py` (`python3 -I -B`).

## Outcome: B (no host-state difference found), plus one sharp fingerprint

**No host-level cause of session 2's fast state is observable in the existing telemetry.** Thermals, CPU
throttling, MCE/EDAC, storage errors, services, cron, memory, scheduler queue, pressure stall, disk service time
and per-CPU load show nothing that is unique to session 2 and that could be a cause. What the data does show is
where the difference lives:

> **In all 5 of session 2's capacity runs the server batches more: frames/recv 15.6 to 15.8 and frames/send 17.5
> to 17.9. In the other 20 capacity runs they are 13.2 to 13.9 and 14.6 to 15.6. The two ranges do not touch.**
> At the fixed 654,569 ops/s anchor the batching is identical in all five sessions (1.2 / 1.4).

This is the saturation / batching signature the review suspected. It is an observed state, not yet a cause: the
server and `net_bench` are restarted for every run, so whatever selects the state persists across processes within a
session (5 of 5 runs fast, 20 of 20 others slow), and nothing in the host telemetry says what.

## What was available (and what was not)

- sysstat: `/var/log/sysstat/sa12`, copied unchanged to `raw/sa12` (sha256 in `raw/sa12.sha256`; the live file
  keeps growing, this copy is as of 22:10 UTC). Interval **10 minutes**, `SADC_OPTIONS="-S DISK"`. A 984 s session
  load window (12 runs) overlaps only 2 or 3 samples, and each sample is a mean over its interval, so **only
  session-wide means are recoverable. A 60 s capacity run cannot be resolved.**
- **Not recorded, so not invented:** CPU frequency (`sar -m FREQ` not in the file), temperature sensors, per-CPU
  interrupts, softirq per CPU, process accounting (`acct` not installed). `ipmitool` is not installed, so the BMC
  event log was not read.
- Method: load-window mean = overlap-weighted sample mean with the idle-settle baseline subtracted from the part of
  each sample outside the window. Instantaneous fields (memory, run queue, PSI) are quoted as samples, not averaged.
  Discard activity is taken from the exact A1 diskstats (0 sectors discarded in every load window); sysstat discard
  columns are not used because a sample can contain the *next* session's fstrim.
- "LOO" = (session value minus median of the other four) / max(range of the other four, a resolution floor). The
  others' spread is tiny (the CVs are around 1%), so LOO flags small but consistent shifts; read it with the
  absolute numbers, not alone.

## 1 and 2. CPU and block device (sysstat), session-wide means

| | k1 | k2 | k3 | k4 | k5 |
|---|---|---|---|---|---|
| CPU %usr (all 72) | 7.37 | **7.49** | 7.35 | 7.36 | 7.36 |
| CPU %sys | 7.93 | **7.81** | 7.93 | 7.92 | 7.94 |
| CPU %soft | 3.04 | **2.96** | 3.03 | 3.02 | 3.03 |
| %iowait / %steal / %irq | 1.45 / 0 / 0 | 1.45 / 0 / 0 | 1.44 / 0 / 0 | 1.45 / 0 / 0 | 1.43 / 0 / 0 |
| context switches /s | 269.9 k | **267.5 k** | 269.9 k | 270.2 k | 269.2 k |
| loopback packets /s | 1.321 M | **1.300 M** | 1.319 M | 1.318 M | 1.320 M |
| loopback kB /s | 284.9 k | **295.3 k** | 284.4 k | 285.1 k | 283.8 k |
| data SSD write kB /s | 261.1 k | **270.6 k** | 259.6 k | 262.1 k | 260.1 k |
| data SSD tps | 36.9 k | **36.5 k** | 37.0 k | 37.0 k | 36.9 k |
| data SSD %util | 64.98 | 64.85 | 64.94 | 64.84 | 64.79 |
| data SSD await ms | 0.102 | 0.073 | 0.108 | 0.103 | 0.074 |
| data SSD aqu-sz | 2.330 | 2.342 | 2.327 | 2.326 | 2.326 |
| server CPUs 2..16 busy % | 82.56 | 82.58 | 82.49 | 82.54 | 82.66 |
| client CPUs 18..32 busy % | 79.91 | **79.36** | 79.64 | 79.67 | 79.73 |

Reading: session 2's host-wide shifts are small (0.1 percentage point on 72 CPUs) and all point the same way as
the benchmark result: a little less kernel time (%sys, %soft, context switches), fewer but larger loopback packets
(packets/s -1.5%, bytes/s +3.6%, so about 5% more bytes per packet), and more bytes written because more
operations completed. That is the batching fingerprint seen from the host side, i.e. a **consequence** of the fast
state. Disk service time does not separate it: `await` is low in sessions 2 and 5 and high in 1, 3, 4, and session 5
is a normal-throughput session. %util, queue size and the pinned server CPUs' busy % are the same in all five
(saturated server CPUs in every session). Nothing else in the per-CPU data stands out: of the 72 per-CPU series, the
largest session-2 deviations are cpu28 (-3.5 LOO) and cpu27 (+3.3), a client CPU and an unpinned one, at fractions of a
percentage point, with the other sessions showing similar-size deviations on other CPUs (counts in `tables.txt`).

## 3. Thermal, frequency, MCE, storage errors

- Journal 16:00 to 20:05 UTC (`raw/journal-window.txt`): no thermal, throttle, MCE, EDAC, I/O error, reset or
  timeout message. Two kernel notices in the window, `perf: interrupt took too long` at 17:30:43 (session 2) and
  18:56:34 (session 4, a normal session): the perf sample-rate governor lowering itself, not a fault.
- **Current, not historical** (`current-counters.txt`, captured 22:14 UTC; these counters are cumulative since boot at
  15:32:16, so zero bounds the whole experiment window): `core_throttle_count` and `package_throttle_count` 0 on all
  72 CPUs, EDAC `ce_count` / `ue_count` 0 on all four controllers, no MCE or storage reset in the kernel log since
  boot, governors `performance` on all 72, `intel_pstate` passive, `no_turbo` 0.
- SSD temperature (A1 snapshots): 34 to 38 C in every session, max 38; no session-2 difference. The temperature
  excursions logged by `smartd` are on `/dev/sdb` (the OS disk), not the data SSD.
- Not available historically: actual CPU frequency, uncore frequency, package power / power-cap events.

## 4. Services, cron, processes

`events.txt` maps every journal event (excluding the harness's own `sudo` steps and the 10-minute sysstat job) to
each session's settle and load window. Session 2's load window contains **only** the perf-rate notice above;
session 1's contains nothing; sessions 3, 4, 5 contain more background activity than session 2 and are normal:
`cron.hourly` at 18:17:01 (k3), `smartd` at 19:02:18 and a perf notice (k4), `fwupd-refresh` at 19:54:27 (k5). `fwupd-refresh` ran in sessions 1,
2, 4 settle windows; `cron.hourly` at 17:17:01 in session 2's settle and 16:17:01 and 19:17:01 in those of sessions
1 and 5. No service or job overlaps session 2 uniquely. The previous Claude session issued no tool calls between
16:30 and 19:57 (its transcript), so it did not compete for the host. No process accounting exists.

## 5. Consolidated per-session table

| k | anchor p99 median ns | cap thr median | cap CPU us/op | cap write B/op | cap p99 ns | cap frames/recv | cap frames/send | ckpt cap s | SSD C pre/start/end | scan % pre/start/end | trim bytes |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 966,656 | 4,021,146 | 1.833 | 69.2 | 11,796,480 | 13.70 | 15.30 | 4.81-5.01 | 34/34/37 | 79.89/79.96/80.00 | 3,779,301,535,744 |
| 2 | 983,040 | **4,356,828** | **1.700** | 71.1 | **9,699,328** | **15.80** | **17.70** | 4.79-5.01 | 37/35/38 | 80.00/80.08/80.12 | 185,471,426,560 |
| 3 | 999,424 | 4,010,715 | 1.836 | 68.9 | 11,796,480 | 13.60 | 15.20 | 4.82-5.19 | 38/35/38 | 80.12/80.19/80.24 | 195,851,280,384 |
| 4 | 966,656 | 4,033,870 | 1.826 | 69.7 | 11,534,336 | 13.70 | 15.30 | 4.99-5.07 | 38/34/37 | 80.24/80.31/80.35 | 184,920,182,784 |
| 5 | 983,040 | 3,996,693 | 1.842 | 69.7 | 11,796,480 | 13.40 | 15.00 | 4.86-5.00 | 37/34/37 | 80.35/80.42/80.46 | 184,524,406,784 |

Anchor-rate (open loop) quantities are the same in all five sessions: CPU 10.17 to 10.22 us/op, batching 1.2 / 1.4,
ring msgs/request 1.18. Ring messages per request are 1.12 in every capacity run, so the extra batching is on the
socket side (frames per `recv` / `send`), not in the shard rings. Historical CPU-frequency metric: none exists.
Historical disk await / util: rows above. Background scan: active throughout, +0.12 percentage point per session,
no discontinuity; session 2 happens to straddle 80.00 to 80.12 %, which is one observation and not evidence.

## What this does and does not say

- It rules out, for this window: thermal or power throttling (counters zero, no log), memory errors, storage
  faults or resets, a unique background service/cron/process, a different trim or settle (trim 185 to 196 GB in
  sessions 2 to 5, 0 sectors written or discarded in every settle and every load window), SSD temperature, and a
  disk service-time change visible at 10-minute resolution.
- It cannot rule out anything faster than the 10-minute sampling, anything not sampled (CPU and uncore frequency,
  C-states, power capping, BMC events, per-run page and NUMA placement, filesystem and WAL physical layout), nor
  a run-to-run chaotic dynamic that only appears under saturation.
- It does not support attributing the state to SSD GC, background scan or trim (the review's caution stands).

## Proposal (no implementation, no rerun, for review)

Because the discriminating signature is server `frames/recv` and `frames/send` at saturation and it was already
recorded for free, the cheapest next step is **prospective instrumentation, not another A2-a**: log per run, in the
capacity runs only, (a) per-CPU `scaling_cur_freq` / MSR or `turbostat` 1 s samples for the 8 + 8 pinned CPUs and
package power, (b) 1 s `sar`/`/proc` samples (the shipped sysstat interval is 10 min; a `sadc 1` during the runs would
do), (c) `/proc/<pid>/numa_maps` and THP state of the server after warm-up, and (d) the WAL files' extent layout
(`filefrag`) per session. Then one bounded causal characterization could decide between: a frequency/power state, a
memory/page placement state, a filesystem/WAL layout state, or a client/server pacing attractor. Alternatively, treat
the capacity measurement as bimodal and bound it by design. Both are the reviewer's call; neither is started here.
