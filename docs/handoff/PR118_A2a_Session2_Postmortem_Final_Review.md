# PR #118 A2-a session-2 read-only postmortem: final review

Filed verbatim from cody's upload in the project thread, 2026-11-12 22:32 UTC
(relayed because the reviewer's GitHub connector is read-only).

---

PR #118 FINAL REVIEW

Repository:

"codypendant/SNC-server-OS"

PR:

"#118 — A2-a session-2 read-only postmortem: no host-state cause found"

Reviewed head:

"af9862bbbe0fa2539b3c974c9e38588fc8220b10"

Base:

"6c5105b76c89d5709d00269ecf8778db34c84547"

Decision

PR #118: APPROVED.

MERGE AUTHORIZED.

READ-ONLY POSTMORTEM: COMPLETE.

PR #117 A2-a VERDICT REMAINS STOP.

Do not reinterpret the prior STOP as STABLE.

---

Postmortem conclusion

No historical host-state difference observable in the available telemetry explains session 2.

Existing evidence does not identify:

- thermal throttling;
- CPU/package throttle events;
- MCE/EDAC faults;
- storage errors or resets;
- unique background service activity;
- a unique SSD temperature state;
- a unique background-medium-scan transition;
- a checkpoint-duration difference;
- an obvious disk-await/utilization difference.

Historical CPU/uncore frequency and package-power data were not recorded.

Therefore they cannot be ruled in or out.

---

Strong new fingerprint

Session 2's five capacity runs:

frames/recv = 15.6–15.8
frames/send = 17.5–17.9

Other twenty capacity runs:

frames/recv = 13.2–13.9
frames/send = 14.6–15.6

No overlap.

At the fixed 654,569 ops/s anchor:

frames/recv = 1.2
frames/send = 1.4

in every session.

Ring messages/request at capacity:

"1.12"

in every session.

Therefore the separated state is specifically associated with socket/network-loop batching under saturation, not a different shard-ring routing regime.

---

Source-level meaning

The server increments:

"recv_calls"

on actual "recv()" calls and:

"frames_in"

for each complete request frame dispatched.

The reported value:

frames/recv = frames_in / recv_calls

Likewise:

frames/send = frames_out / send_calls

Therefore the session-2 signature means more application frames are handled per socket syscall.

This is capable of reducing syscall/kernel overhead per request.

It is consistent with:

normal capacity CPU/op ≈ 1.83–1.84 us
session 2 CPU/op       ≈ 1.70 us

and:

normal capacity ≈ 4.00 M ops/s
session 2       ≈ 4.36 M ops/s

This is a strong correlation and plausible mechanism.

It is not yet proof of causal direction.

---

Current hypothesis space

The next characterization should distinguish among at least:

1. CPU frequency / package-power state
   
   - frequency or power changes alter event-loop cadence and socket accumulation;

2. scheduler / polling cadence
   
   - the same code enters a different receive/send batching attractor under saturation;

3. client-side send batching
   
   - larger client writes produce larger server receive batches;

4. memory / NUMA / page-layout state
   
   - changes execution cadence enough to alter socket batching;

5. TCP/loopback kernel state
   
   - queueing, GRO/coalescing or other socket scheduling behavior changes the batch size;

6. filesystem/WAL layout
   
   - still possible, though current checkpoint and disk-service evidence does not separate session 2.

Do not select one yet.

---

NEXT AUTHORIZED TASK

Prepare a prospective capacity-state characterization

Prepare and preregister only. Do not run yet.

No SNC product behavior change.

No checkpoint change.

No network-stack tuning.

No socket-option change.

No threshold change.

No competitor.

The characterization should observe the existing phenomenon without trying to force either state.

---

Experimental structure to propose

Use the existing A2-a storage start procedure.

Proposed minimum:

K = 5 sessions

Each session:

1. approved A2-a trim/start procedure;
2. 30-minute clean settle;
3. start anchor at 654,569 ops/s;
4. 3 closed-loop capacity reps;
5. end anchor at 654,569 ops/s.

This is much smaller than the previous 12-run/session experiment while retaining independent storage-reset sessions.

Do not use the result to replace the original A2-a STOP.

Purpose:

capture both saturation states with enough instrumentation to identify what separates them.

---

Required prospective instrumentation

Instrumentation must be observational and identical in every session/run.

A. CPU frequency / execution

Prefer "perf stat" or equivalent already available on the host.

For the pinned server CPUs and client CPUs separately, capture where supported:

- cycles;
- ref-cycles;
- instructions;
- task-clock;
- context switches;
- CPU migrations.

From cycles/ref-cycles or equivalent derive effective frequency behavior where defensible.

If "turbostat" is already installed and usable, package/core frequency and package-power sampling at 1-second cadence is desirable.

Do not install new software merely for this experiment without returning for review.

First determine what is already available.

---

B. Socket syscall counts

This is high priority.

Using an external observation mechanism such as supported "perf" syscall tracepoint counting, capture separately for server and client CPU sets:

- recv/recvfrom syscall count;
- send/sendto syscall count;
- bytes if a low-overhead supported method exists.

Do not use "strace" for the benchmark evidence path unless separately justified; its perturbation is too high.

The existing server-log:

- frames/recv;
- frames/send;

remains mandatory.

The new instrumentation should determine whether the client side also changes its syscall batching.

---

C. 1-second host sampling

The prior 10-minute sysstat archive is too coarse.

Capture at approximately 1-second cadence during each measured capacity run:

- per-pinned-CPU utilization;
- run queue;
- context switches;
- softirq;
- loopback packets/s;
- loopback bytes/s;
- approved SSD I/O;
- PSI where available.

Use an isolated evidence sampler that does not modify system configuration.

Record its CPU placement.

Do not place the sampler on a benchmark CPU.

---

D. Memory / NUMA

For the server process, capture at a fixed preregistered point after warm-up:

- "/proc/<pid>/numa_maps";
- "/proc/<pid>/status";
- "/proc/<pid>/smaps_rollup" where available;
- THP-related state relevant to the process.

We need to know whether the alternate saturation state accompanies different page placement.

---

E. Socket/kernel state

Before each session and, where safely observable, during a capacity run capture:

- relevant "ss -tin" state;
- loopback interface offload configuration;
- relevant TCP socket-buffer sysctls;
- RPS/XPS/softnet configuration.

These are observations only.

No settings change.

---

Tool-availability precheck

Before writing the final preregistration, determine read-only availability of:

- "perf";
- required perf tracepoints/events;
- "turbostat";
- 1-second "sar"/"sadf" capability;
- "/proc" files above.

No performance run is authorized by this availability check.

If a required observation would need package installation or host reconfiguration, return that fact instead of doing it automatically.

---

Classification — fixed before data

The characterization is not a PASS/FAIL benchmark.

Each capacity run should be classified descriptively after data as:

Large-batch state

using a frozen criterion derived from the already observed non-overlapping ranges.

Recommended preregistration boundary:

frames/recv >= 15.0

The historical states are:

normal: 13.2–13.9
fast:   15.6–15.8

15.0 lies in the observed gap and is not selected from new data.

Also record frames/send but do not create a second post-hoc classifier.

Normal-batch state

frames/recv < 15.0

This classifier is for causal characterization only.

It does not alter any performance verdict.

---

Analysis objective

For every instrumented metric, ask:

Does it separate all observed large-batch runs from normal-batch runs?

Particularly inspect:

- effective CPU frequency;
- cycles/op;
- instructions/op;
- recv syscall/request;
- send syscall/request;
- client syscall/request;
- scheduler cadence;
- softirq;
- loopback packet size;
- NUMA placement.

Do not search hundreds of metrics and declare whichever has the largest difference causal.

Primary hypotheses and metrics must be preregistered before execution.

---

Result discipline

After the characterization:

- preserve every run;
- no replacement session;
- no tuning;
- no forcing a large-batch or normal-batch state;
- no product change.

Return the evidence for review.

If both states occur and one preregistered variable cleanly separates them, we can authorize a causal test.

If only one state occurs, return that result; do not keep running until the other state appears.

If no measured variable explains the two states, we will decide whether to treat saturation capacity as intrinsically bimodal for this architecture/environment.

---

Still unauthorized

- running the prospective characterization before its preregistration is reviewed;
- changing "ConnSet";
- changing socket buffer sizes;
- changing poll cadence;
- changing "TCP_NODELAY";
- changing batching behavior;
- stable-peak bracket;
- new Profile-A sweep;
- duration Latin square;
- CRITERIA freeze;
- competitor runs;
- REFINE #2.

---

Final ruling:

PR #118 APPROVED.

MERGE IT.

THE A2-a STOP REMAINS.

NEXT: PREREGISTER A SMALL, PROSPECTIVELY INSTRUMENTED CAPACITY-STATE CHARACTERIZATION. DO NOT RUN IT YET.