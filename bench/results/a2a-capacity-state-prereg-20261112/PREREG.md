# A2-a capacity-state characterization: PREREGISTRATION (revision 2; NOT RUN; for review)

Authority: `docs/handoff/PR118_A2a_Session2_Postmortem_Final_Review.md`, NEXT AUTHORIZED TASK. This document is written
before any data. **Nothing in it has been run**; the only measurements taken are the 1 to 3 s idle-host availability
probes in `precheck/availability.txt` (no benchmark, no SNC process, no configuration change, no package installed).

It does **not** replace, re-judge or soften the A2-a **STOP** (PR #117). It is a causal characterization, not a PASS/FAIL
benchmark. No SNC product, checkpoint, socket-option, poll-cadence, `ConnSet`, buffer or threshold change. No competitor.

## 0. Reviewer decisions (revision 1 items, resolved 2026-11-12) and the revision-2 correction

1. Exact-command `sudoers` for `perf`: **APPROVED** (not installed until the run step). 2. `perf` `msr/aperf,mperf,tsc` and
`power/energy-*` instead of `turbostat`: **APPROVED**. 3. Observer effect: **ACCEPTED** as a limitation; the instrumentation is
identical in every run and the result reports it as a potential observer effect. 4. **K = 5 is kept**; K is not raised to
improve the chance of catching the fast state; if only one state appears the experiment stops as preregistered. 5. `epoll_wait`
is **not** traced: **APPROVED**.

**Revision 2 correction (reviewer): capacity and anchor runs have different timelines.** Checked against the code and the A2-a
logs: `S0`/`S1` are open-loop runs with `--warmup 10 --seconds 60 --drain-s 10` (`pilot_plan.json` `warmup_s` 10, `drain_s` 10;
`net_bench` runs the first 10 s of due ops but does not measure them), so their measured window is **t0 + 10 s to t0 + 70 s**;
`C1..C3` are closed-loop `--seconds 60` with no warm-up, measured window **t0 to t0 + 60 s**. Section 3 now defines a measured-window
start `t_m` per run kind and aligns everything to it; the instrumentation duration is a uniform 80 s so the anchors' drain tail
is covered by the same commands. The causal analysis uses the capacity runs only; anchors are same-instrument context.

The original revision-1 items follow for the record (the decisions above supersede them):

1. **Root access for `perf`.** `kernel.perf_event_paranoid` is 4, so `perf` needs root. Rather than change the sysctl
   (host reconfiguration), I propose the same mechanism A2-a used: exact-command `sudoers` entries
   (`sudoers-snc-capstate.proposed`, three `perf stat` commands, fixed arguments, **not installed**). Install only after
   review.
2. **`turbostat` is unusable on this host** (it aborts with "Insanely slow TSC rate, TSC stops in idle?", at idle and under a
   busy loop; see `precheck/availability.txt`). Substitute, all already installed and verified: `perf` `msr/aperf/`,
   `msr/mperf/`, `msr/tsc/` (effective frequency = TSC MHz x aperf/mperf; verified 3.40 GHz on a busy pinned CPU, agreeing
   with cycles/ref-cycles) and `power/energy-pkg/`, `power/energy-ram/` (package and DRAM energy). Nothing is installed.
3. **Observer effect.** The original A2-a runs had no instrumentation. This one adds three counting `perf stat` processes
   (hardware + two syscall tracepoints), a 1 s `sar`, and a few snapshots, all on non-benchmark CPUs. They are identical in
   every run, but they could shift the state mix (or suppress the large-batch state altogether). I cannot test that
   without running. Reported as a limitation in the result either way.
4. **Chance of seeing only one state.** In A2-a, 1 of 5 sessions was in the fast state. If that rate were 0.2 per session,
   the chance of **no** large-batch session in K = 5 is about 33% (0.8^5). K = 5 is as authorized; the reviewer may raise it.
   Per the review, if only one state occurs the result is returned as such and nothing is run until the other appears.
5. `epoll_wait` is **not** traced. The server polls it with timeout 0 (`ConnSet::poll`), a hot loop with a very high call
   rate, so a tracepoint there would perturb the thing observed. Polling cadence is inferred from instructions/op and
   cycles/op instead (H2).

## 1. Identity (frozen; same as A2-a)

Same plan, manifest e, binaries and device identity as PR #116/#117: `snc_server` `69a51b69…`, `net_bench` `9fc54944…`,
`bench/pilot_plan.json` `568e9874…`, `bench/manifests/r640-main-20261111.txt` `768056bb…`; data SSD UUID
`3bf857c8-0cb4-4c57-b9a5-14328e70b061`, Samsung ARFX3840S5xnFTRI, serial S48LNA0M700325, WWN 0x5002538b097f68a0, resolved
by persistent identity (never by the `sdX` letter). Run from the merged `main` HEAD at execution time (recorded), clean tree;
`bench/pilot.py` (frozen at `4250b32`) imported unchanged. Governors `performance`, `numa_balancing` 0, `fstrim.timer`
disabled, per-session guard as in `a2a.py`. Server on CPUs 2,4,...,16, client on 18,20,...,32 (NUMA node 0), unchanged.

## 2. Design

K = 5 sessions, each a fresh storage reset exactly as A2-a steps 0 to 5 (guard, clear data root and sync, A1 pre-trim
snapshot, explicit `fstrim`, proven and synced, 30 min idle settle judged 0 / 0 sectors, A1 start snapshot), then **5 runs**
in fixed order, Profile A, same shapes as A2-a:

`S0` (open loop at 654,569 ops/s) , `C1`, `C2`, `C3` (closed-loop capacity), `S1` (open loop at 654,569 ops/s), then the A1
end snapshot. 25 runs in total. Expected duration about 3.5 h (each run waits for its 80 s samplers). Any execution-integrity failure aborts the whole run
INCOMPLETE: no replacement session, no rerun.

The driver (`capstate.py`, **not written yet**) will import `a2a.py` / `pilot.py` unchanged and wrap `Pilot.one` the way
A2-a does; it, its offline tests and a mutation check come in a separate slice after this design is reviewed.

## 3. Instrumentation (observational, identical in every one of the 25 runs, never touching any setting)

**Run window (revision 2).** The measured client is the `net_bench` started with `--seconds 60` (the load phase uses
`--seconds 0`). The driver polls `/proc` every 50 ms for that process; its appearance is **t0**, and every run-level sampler is
started at t0. The measured-window start **t_m** differs by run kind:

| run | client shape | t_m | measured window | analysed intervals (1 s, after t0) |
|---|---|---|---|---|
| `C1..C3` capacity | `--depth D --seconds 60`, no warm-up | t0 | t0 .. t0+60 | **1..60** |
| `S0`, `S1` anchor | `--rate 654569 --warmup 10 --seconds 60 --drain-s 10` | t0 + 10 s (`plan.warmup_s`) | t0+10 .. t0+70 | **11..70** (1..10 = warm-up, recorded, not analysed; 71..80 = drain, recorded) |

The measure client's own load step is about 0.01 s (the ids file already exists), so t0 is within 0.2 s of the true start; the
measured offset (client exit time minus t0 minus warm-up minus window) is recorded per run. Every sampler runs **80 s** in every
run (identical commands; covers the anchors' 10 s warm-up + 60 s window + up to 10 s drain), the next run does not start until
every sampler has exited, and the raw 1 s series are kept whole so nothing is cut at capture time.

**Sampler placement.** Samplers and `sudo`/`perf`/`sar` run under `taskset` on CPUs **34 and 70** only (one physical core,
node 0, neither a benchmark CPU nor an SMT sibling of one: the siblings of 2..16 are 38..52 and of 18..32 are 54..68). The
placement is recorded in the evidence. Nothing runs on CPUs 2 to 32 (even) or their siblings.

| id | what | mechanism |
|---|---|---|
| A1 | server-set hardware and software counters, 1 s | `perf stat -a -C 2,4,6,8,10,12,14,16 -e cycles,ref-cycles,instructions,task-clock,context-switches,cpu-migrations,msr/aperf/,msr/mperf/,msr/tsc/,syscalls:sys_enter_recvfrom,syscalls:sys_enter_sendto -I 1000 -x, -- sleep 80` |
| A2 | client-set, same events | same with `-C 18,20,22,24,26,28,30,32` |
| A3 | package and DRAM energy, 1 s | `perf stat -a -e power/energy-pkg/,power/energy-ram/ -I 1000 -x, -- sleep 80` |
| B | recv/send syscalls | the two tracepoints in A1/A2 (counting only; no `strace`, no `perf trace`, no `bpftrace`); bytes are not captured (no supported low-overhead method) |
| C | 1 s host sampling | `sar -o <file> -u -P 2,4,...,32 -q -w -B -d -n DEV,SOFT 1 80` (per-pinned-CPU utilization, run queue, context switches, softirq, `lo` packets/s and kB/s, the approved SSD by its kernel name resolved at run time, evidence only), plus `/proc/pressure/{cpu,io,memory}` and `/proc/softirqs` at t_m and t_m+60 s |
| D | memory / NUMA | at **t_m + 30 s** (preregistered; t0+30 s for capacity runs, t0+40 s for anchors), for the server pid and the client pid: `/proc/<pid>/numa_maps`, `/proc/<pid>/status`, `/proc/<pid>/smaps_rollup`; plus `/sys/kernel/mm/transparent_hugepage/{enabled,defrag}` and `AnonHugePages` / `Mems_allowed`; `filefrag` extent counts of the live WAL segment files |
| E | socket / kernel | before each session: `ethtool -k lo`, `/sys/class/net/lo/queues/*/rps_cpus`, the TCP and `net.core` sysctls named in `precheck/availability.txt`, `nstat -az` snapshot; at t_m and t_m+60 s: `nstat -az` (delta); at t_m+30 s: `ss -tin` for the server ports |
| F | existing | server-log `frames/recv`, `frames/send`, ring msgs/request, `recv`/`send` totals (**mandatory**); `pilot.py` per-run `cpu_us_per_op`, `write_bytes_per_op`, latencies; A1 diskstats and SMART per session |

Per-run derived quantities use **ops = exec_ops + index_ok** (the pilot's total-ops numerator), over the 60-interval measured window.
Validity check on the instrument itself: the server-set `sys_enter_recvfrom` count over the capacity window (intervals 1..60) must equal
`requests / (frames/recv)` from the server log within 3%; a larger gap is reported as an instrument defect (the run is
kept, its tracepoint columns are flagged).

## 4. Classification (frozen now, before any data)

Per capacity run (`C1` to `C3`): **large-batch** iff server-log `frames/recv >= 15.0`; **normal-batch** iff `< 15.0`. Boundary
from the A2-a gap (normal 13.2 to 13.9, fast 15.6 to 15.8; 15.0 lies in the gap and is not chosen from new data). `frames/send`
is recorded but there is **no second classifier**. Anchor runs are not classified. The classifier is for characterization
only; it changes no verdict. A session's class is descriptive (how many of its 3 capacity runs fall in each state).

## 5. Hypotheses and primary metrics (frozen; nothing else is eligible to be called causal)

At most two primary metrics per hypothesis, each defined over the 60-interval measured window (above) of the **capacity** runs:

| H | hypothesis | primary metrics |
|---|---|---|
| H1 | CPU frequency / power state | P1a server-set effective MHz = TSC_MHz x sum(aperf)/sum(mperf); P1b package power W (energy-pkg rate, both packages summed) |
| H2 | scheduler / polling cadence | P2a server-set instructions per op; P2b server-set context switches per second |
| H3 | client-side send batching | P3a client-set `sendto` per op; P3b client-set `recvfrom` per op |
| H4 | memory / NUMA / page layout | P4a fraction of server resident pages on node 1 (numa_maps `N1=` / total); P4b server `AnonHugePages` kB |
| H5 | TCP / loopback kernel state | P5a `lo` bytes per packet (sar `-n DEV`: rxkB x 1024 / rxpck); P5b `TcpExtTCPAutoCorking` per op |
| H6 | filesystem / WAL layout | P6a live WAL segment extent count per file; P6b run `write_bytes_per_op` (pilot) |

Also computed for H2/H1 context (labelled secondary, **not eligible**): cycles/op, IPC, CPU migrations, per-CPU utilization,
softirq, `ss -tin` fields, PSI, SSD await/util.
**Negative controls** (expected unrelated; reported with the same rule): N1 SSD temperature at A1 start; N2 session index.
If a negative control also separates the states, no separation is informative.

## 6. Analysis rule (descriptive; frozen)

For each primary metric and negative control, take the per-run values over all capacity runs. If at least 2 large-batch and 2
normal-batch capacity runs exist, report: the two ranges, whether they are disjoint, and the gap relative to the larger
within-state range ("separates" iff disjoint and gap >= the larger within-state range). No p-values, no model fitting, no
searching the secondary metrics for a winner. State explicitly how many large-batch **sessions** exist, because runs in one
session may not be independent (A2-a: 5 of 5 fast runs were one session). With only one large-batch session, a separation is
**candidate only** and a causal test would need its own preregistration. Outcomes, returned for review with all evidence:
both states and a clean separation; both states and none; one state only (**stop**, do not run again); instrument or
integrity failure.

## 7. Result discipline

Preserve every run; no replacement session, no rerun, no tuning, no forcing either state, no post-hoc classifier or
threshold, no hand-edited results. Commit evidence under `bench/results/a2a-capacity-state-r640/` from an out directory kept
outside the tree and copied in unchanged. The result does not touch the A2-a STOP. Stable-peak bracket, Latin square, CRITERIA,
competitors, REFINE #2 remain unauthorized.

## 8. Availability findings (read-only precheck; details in `precheck/availability.txt`)

Available: `perf` 6.8.12 (root only; counting for hardware, `msr/*`, `power/*`, and `syscalls:sys_enter_recvfrom` / `sendto`
verified on the exact CPU sets, 100% enabled time, no multiplexing); `sar`/`sadf` 12.6.1 with 1 s `-o` capture and
`-n DEV,SOFT`; `nstat`; `ss`; `ethtool`; `numactl`; `/proc/<pid>/{numa_maps,status,smaps_rollup}` readable for own processes; THP
`madvise`. Intel uncore frequency `current_freq_khz` is root-only (min = max = 2.4 GHz is readable: pinned). Not usable:
`turbostat` (above); `perf stat --timeout` cannot combine with `-I`, hence the fixed `sleep 80` workload. Needs reviewer
action: the `sudoers` entries above. Needs no installation: everything else.
Server and client call `recv` / `send` / `epoll_wait` directly (`src/net/conn_set.cpp`, `src/net/client.cpp`), so
`sys_enter_recvfrom` / `sys_enter_sendto` are the right tracepoints; TCP_NODELAY is already set on both ends.

## 9. Driver (revision 2: written, tested offline, NOT run)

`capstate.py` (run / analyze), `test_capstate.py` (starts no process; `python3 -I -B test_capstate.py` prints `ALL OK`),
`mutate_capstate.py` (21 of 21 mutants caught), `fixtures/` (real-format captures taken on this host for the parsers).

- It imports `a2a.py` / `pilot.py` unchanged and reuses `a2a.session()` for the storage reset; only the run order
  (`S0 C1 C2 C3 S1`) and the phase name (`capstate`) differ. The instrumentation is injected by wrapping `pilot.subprocess.run`:
  only the measured `net_bench` (`--seconds` > 0) is intercepted, the load step and everything else pass through.
- **Implementation note on t0:** because the driver launches the measured client itself (through that wrapper), t0 is the
  launch time of the client process, not a 50 ms `/proc` poll; the timeline in section 3 is unchanged (same under-0.2 s offset,
  recorded as `offset_s`). t_m, the snapshot times (t_m, t_m+30, t_m+60) and the analysed intervals are as in section 3.
- A sampler that exits non-zero is an execution-integrity failure (INCOMPLETE, no replacement). A missing snapshot is recorded
  (`UNREADABLE` / `NO PID` text in its file), not fatal. The instrument validity check (recvfrom vs the server log, 3%) is
  computed per run by `analyze` and flagged, not fatal.
- Preflight refuses unless: the sudoers file equals the driver's perf commands, `sudo -n -l` accepts each, the CP-1A hashes and
  the A2-a checks hold, the tools exist, and the sampler CPUs are free. The sudoers file is **not installed** until the run step.
- `analyze` implements section 5 (the twelve primary metrics and two negative controls), the section 4 classifier and the section 6
  rule; it writes `characterization.json` and prints no verdict. It was written and tested before any data, so it cannot be tuned
  to them.
- **Not exercised offline** (as with a2a.py): real `perf`/`sar`/`sudo`/`taskset`, `pilot.Pilot` preflight, the real server and
  client. The first real exercise is the run itself; any failure there is an integrity failure, not a reason to adapt the plan.
