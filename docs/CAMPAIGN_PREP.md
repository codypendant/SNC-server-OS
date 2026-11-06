# Campaign preparation: manifests and the first-wave gap inventory

**Status (2026-11-06):** work order `handoff/PR53_Review_Handoff.txt`;
plan corrected per the PR #54 review (`handoff/PR54_Review_Handoff.txt`):
the pilot is SNC-only, CP-1 is split into CP-1A and CP-1B, and SpacetimeDB
stays in workload A's comparative set. **Stopped here until the R640 is
available**: CP-1A is not authorized yet.

- **CORRECTNESS BASELINE: READY FOR CAMPAIGN PREPARATION.**
- **FULL EXPERIMENTAL BASELINE: NOT YET FROZEN.**

This document covers:

1. what the main R640's manifest records, and how to capture it;
2. the first-wave gap inventory;
3. a minimal plan, split into the SNC pilot harness (CP-1A) and the
   comparative harnesses (CP-1B);
4. the resource metric definitions, NUMA-aware allocation, the Redis
   index decision and the complexity inventory (sections 4 to 7);
5. the order of work to the freeze.

It authorizes nothing by itself, and it contains no comparative
results. Every gap below is read from the repository (INFERRED, EL0).

## 1. Main R640 manifest

**Captured 2026-11-06, untouched:**
`bench/manifests/r640-main-20261106.txt`, measured on the host itself by
`bench/manifest.sh` at 58c638f. The topology review and the PROPOSED
pilot allocation are in [R640_TOPOLOGY.md](R640_TOPOLOGY.md), waiting on
review. The procedure below stays for the remote R640 and for any
re-capture after a hardware, BIOS or kernel change.

**What `bench/manifest.sh` records** (extended in this slice to cover
the work order's list):

| Work order item | Recorded by |
| --- | --- |
| CPU model, topology; sockets, cores, threads | `lscpu`, `lscpu -e` |
| NUMA | `numactl --hardware`; per-node memory and huge pages (`/sys/devices/system/node`) |
| Memory | `free -h`, `/proc/meminfo`; BIOS and DIMMs (size, speed, rank) through `dmidecode` when run as root |
| OS, kernel | `uname -a`, `/etc/os-release`, `/proc/cmdline`, `numa_balancing`, `swappiness`, dirty ratios, isolated and `nohz_full` CPUs |
| Storage, filesystem, mounts | `lsblk`, `nvme list`, the data directory's `df`/`findmnt` (mount options), and per disk: scheduler, write cache, FUA, queue depth |
| NIC, link | `ethtool` (speed, driver, firmware), `ip -br link`, MTU, the NIC's NUMA node |
| IRQ, RSS | `ethtool -l` (channels), `-x` (RSS indirection), `-k` (offloads), `-c` (coalescing), `-g` (rings); each NIC IRQ's CPU affinity; whether irqbalance is running |
| THP, huge pages | THP `enabled` and `defrag`; `HugePages_*` |
| Governor, frequency | the governor per CPU; the frequency driver, min/max, `intel_pstate` status and `no_turbo`, boost; idle states and their latencies |
| Compiler, build environment | `c++`, `cmake`, `ninja`, `python3`, glibc versions |
| Experiment network topology | a section the operator fills in by hand: which hosts share which switch or link, link speeds, and each host's role |

**To run on the R640** (as root, from a checkout of `main`; NAME, DATA_DIR
and the NIC names are the host's own):

```sh
cd SNC-server-OS && git pull     # your checkout of the repository
sudo apt-get install -y numactl ethtool nvme-cli dmidecode pciutils util-linux   # Debian/Ubuntu; dnf on RHEL-likes
ip -br link                      # pick the test NIC(s)
findmnt /                        # pick the filesystem the SNC data dirs will use
sudo sh bench/manifest.sh r640-main /path/to/snc-data eno1 \
  > bench/manifests/r640-main-$(date -u +%Y%m%d).txt
```

Capture the host **untouched**: no CPU, NUMA, IRQ, THP or governor
tuning before this first manifest. If the experiment later tunes any of
them, the campaign configuration records the changed settings, and a
second manifest is taken.

The script is best-effort evidence collection: a missing tool or an
unsupported option (it varies by NIC driver, distribution and kernel)
is printed as unavailable and never stops the script. A missing value
means "not recorded", not a hardware property. Check the output for
obvious omissions before committing it.

Then fill in the topology section by hand. Remove any public address of
a host that is not disposable. Commit the file, or send it back for
review.

**Remote R640:** the same procedure, plus RTT, jitter and loss to the
main host (`bench/lab/lab.py inventory p0`). It is due before any WAN or
remote-site run, not before a local pilot that does not use it.

## 2. First-wave gap inventory

**Two phases, kept apart (PR #54 review).**

- **The within-system pilot is SNC-only.** No PostgreSQL, Redis or
  SpacetimeDB number is produced before `CRITERIA.md` is committed:
  a "pilot per system" would be comparative data before
  pre-registration. The pilot runs workload A at fixed settings on the
  R640 to find:
  - a stable run duration;
  - the object count and working-set range;
  - the repetitions needed for an acceptable CV;
  - a p99 objective realistic for SNC's own stable envelope;
  - the ranges of the Path B dimensions;
  - how the resource sampling behaves;
  - whether the Profile A and B harnesses are correct;
  - whether the R640 topology and pinning plan behave as expected.
- **Noise floor and objective (EXPERIMENTAL_BASELINE 10.1 and 10.2,
  reconciled in the PR #55 review):**
  - N = max(3 × max(CV_throughput_SNC, CV_p99_SNC), N_MIN), from the
    SNC pilot. N_MIN is set at the criteria-setting review, after the
    pilot and before any competitor run.
  - Competitor variance is governed by a pre-registered V_MAX. A
    competitor cell whose CV exceeds V_MAX is invalid, and is rerun or
    has its environment corrected. N never moves.
  - The p99 objective comes from an SNC-only load sweep: SNC's p99 at a
    fraction of its stable peak that is declared before the sweep. It
    never moves because of a competitor's result.
  - N, N_MIN, V_MAX, the p99 objective, M_B and the Path B thresholds
    are all in `CRITERIA.md` before any competitor performance run. No
    value is chosen yet.
- **Workload A's comparative set** stays as EXPERIMENTAL_VALIDATION names
  it: SNC, SpacetimeDB, PostgreSQL plus a service, and Redis plus a
  service. SpacetimeDB does not block the pilot; its harness must exist
  before `CRITERIA.md` and the first comparative wave of workload A.
- **Second wave:** workloads B and C (X2), Orleans, Restate and DBOS
  (X3), and WAN once the remote R640's manifest exists.

| # | Gap | Exists today | Phase | Class |
| --- | --- | --- | --- | --- |
| G1 | **SNC pilot runner:** fixed workload A settings repeated, CV of throughput and of p99, raw output kept | Single runs: `net_bench`, `lab.py` | CP-1A | **REQUIRED** |
| G2 | **Competitor tail latency.** `run.py` reads only pgbench's *average* latency and Redis's p50; the primary metric needs p99 and p99.9 | SNC: `net_bench` prints p50 to p99.9 | CP-1B | **REQUIRED** before the comparison |
| G3 | **Resource metrics**, defined in section 4 | Pieces: SNC `RESOURCES` (`rss_kb`), `bench/lab/cpu_sample.py` | SNC: CP-1A; competitors: CP-1B | **REQUIRED** |
| G4 | **X4 tagging** of Profile B intervals | `lab.py` polls STATUS; no tagging rule | CP-1A | **REQUIRED** before any Profile B result |
| G5 | **Index-query mix in the SNC driver** (X1 put `INDEX` on the wire; no driver sends it) | `net_bench` mixes reads, deposits and transfers | CP-1A; competitor side CP-1B | **REQUIRED** for workload A |
| G6 | **Local/remote ownership knob** | `net_bench --route owner/any`, `--misroute PCT`; in-process sweeps (CROSSCORE.md) | n/a | **EXISTS** for the pilot's fixed settings |
| G7 | **NUMA-aware allocation**, section 5 | CPU pinning: `net_bench --cpus`, `snc_server --cpus`, `run.py --server-cpus/--client-cpus`; no memory binding or limit | SNC: CP-1A; wrapper for all: CP-1B | **REQUIRED** |
| G8 | **Semantics and mismatch table**, including the Redis index decision (section 6) | CONVENTIONAL.md, REPLICATION.md prose | Before CRITERIA.md | **REQUIRED** (document) |
| G9 | **Complexity inventory**, observable counts only (section 7) | None | Before CRITERIA.md | **REQUIRED** (document) |
| G10 | **SpacetimeDB workload A harness** (X3) | None | CP-1B | **REQUIRED** before CRITERIA.md and the first workload A comparison; not before the pilot |
| G11 | **Workload B and C drivers** (X2) | Primitives only: `create_near`, follow-ups, slot moves, `resume` | Second wave | DEFERRED |
| G12 | **Orleans, Restate and DBOS harnesses** (X3) | None | Second wave | DEFERRED |
| G13 | **Disk-full/EIO injection; randomized long fault runs** | Lab P3 to P6, soak | Failure program | DEFERRED |

## 3. Plan (for review; nothing here is built)

No change to `snc_server`, the witness, or the wire in either slice.
`INDEX` semantics stay as X1 built them.

**CP-1A: SNC pilot harness.** May be authorized after the manifest
review.

- G1: `bench/pilot.py`. It runs SNC at fixed settings at least 5 times,
  pinned per section 5. It also runs the offered-load sweep that 10.2's
  knee rule needs. That rule is an exact, deterministic algorithm, written
  down before the R640 sweep runs, so that the same data always gives the
  same knee and objective (PR #56 review). It keeps every run's raw output and writes
  `pilot.json`, with per-run throughput, p50, p95, p99, p99.9, the CV of
  throughput and of p99, and the section 4 metrics.
- G3, SNC side: the section 4 metrics for `snc_server` processes.
- G4: X4 tagging in `lab.py`, classified from the STATUS samples it
  already takes:
  - healthy and READY;
  - degraded or lost standby (`degraded=1`, `blocked=1` or `role=n`);
  - failover, recovery, or not READY.
  The summary reports time and operations per class. A Profile B
  HA_DURABLE figure comes only from the healthy class; degraded
  (LOCAL_DURABLE) intervals are never relabelled as HA performance.
- G5: `net_bench --index PCT`. That share of operations is `INDEX`
  lookups on the email index. Index queries are reported apart from the
  rest of the mix: count, successes, refusals, errors and their own
  latency percentiles.
- G7, SNC side: explicit CPU lists and memory binding (section 5).
- **Tests:** unit tests for the CV, tagging and per-operation
  arithmetic, and a loopback smoke run on the dev VM. Those results are
  correctness checks only.

**CP-1B: comparative harness preparation.** After the SNC pilot, before
`CRITERIA.md`.

- G2: PostgreSQL p99 and p99.9 from pgbench's per-transaction log (`-l`),
  and Redis's from its percentile output.
- G3, competitor side: the same accounting boundary as SNC (section 4).
  PostgreSQL's backends come and go, so its boundary is a process tree
  or a cgroup, not a fixed list of PIDs.
- G7: one allocation wrapper for every system (section 5).
- G5, competitor side: a PostgreSQL index on email, and the Redis
  decision of section 6.
- G10: the SpacetimeDB workload A harness, with its semantics stated.
- G8 and G9, the documents.
- Building a harness is not a comparative result. While CP-1B is built,
  only tiny functional smoke tests run on the dev VM, as correctness
  checks. No cross-system performance comparison runs, and no smoke
  number is used as evidence.

## 4. Resource metric definitions (G3)

Each system has a **system boundary**: the server-side processes that
make up the system under test, listed in the run's configuration. For
SNC that is the `snc_server` processes, plus `snc_witness` in Profile B.
For PostgreSQL it is the postmaster and its backends, plus the service
process if the comparison includes one. For Redis it is `redis-server`,
plus the service. Load generators are never inside it. Every system uses
the same boundary rules; a mismatch that cannot be avoided is written
next to the result.

| Metric | Definition | Caveat |
| --- | --- | --- |
| **CPU per op** | User plus system CPU time (`/proc/<pid>/stat`, `utime + stime`) of every process inside the boundary, over the measured window, divided by the successful operations in that window | Children that exit inside the window are counted through `cutime`/`cstime`, or the run states that they are not |
| **Memory per object** | **Primary: incremental steady-state RSS**: the boundary's summed RSS at steady state, minus the same processes' RSS when they are empty (started, no objects loaded), divided by the object count. **Peak RSS is reported separately** (`VmHWM`) | RSS counts shared pages in every process that maps them. Multi-process systems (PostgreSQL) also report PSS (`/proc/<pid>/smaps_rollup`) |
| **Storage bytes per op** | **Host-visible write I/O**: `write_bytes` from `/proc/<pid>/io` over the window, summed over the boundary, divided by operations. Labelled as host-visible I/O, **not physical media bytes** | Device-level bytes (`/sys/block/<dev>/stat`) are reported only when the device holds nothing but the system under test, and are labelled as such. Write amplification inside the device is not measured |
| **Network bytes per op** | Interface counters (`/sys/class/net/<if>/statistics/{rx,tx}_bytes`) of the test NIC over the window, minus an idle baseline of the same length measured just before, divided by operations | **Loopback pilot runs give no physical-NIC evidence.** Loopback bytes are reported as loopback, never as network cost. Physical numbers come only from runs whose clients are on another host |

## 5. Allocation on the R640 (G7)

The manifest gives the topology. Each run's configuration then pins
explicitly and records:

- the exact logical CPUs given to the servers, and their physical cores,
  socket and NUMA node (`lscpu -e`);
- the client CPUs, which never overlap the server CPUs;
- the memory binding policy (`numactl --cpunodebind=N --membind=N`, or
  the equivalent);
- the IRQ placement of the test NIC relative to the server and client
  cores, when a NIC is involved;
- whether automatic NUMA balancing is off (`kernel.numa_balancing=0`)
  for the run. If it is not off, the run says so.

**Single-socket (local-NUMA) runs** keep the server's memory on its CPU's
node. **Cross-NUMA is a named experimental dimension**, set on purpose,
never left to the scheduler. Every system in a comparison gets the same
CPU list, node, memory limit, storage device and network path.

## 6. Redis index semantics (decision before the comparison)

Workload A includes secondary-index queries. Plain Redis has no
secondary-index primitive like SNC's or PostgreSQL's. Before any
comparative test, cody and Codex choose one of these, and the semantics
table states it plainly:

- **A.** Redis plus a named index structure that the service maintains,
  with its write and update cost included in every mutation.
- **B.** The Redis index-query cell is marked a semantic mismatch. That
  metric is left out of the direct matched comparison, and the rest of
  the mix runs without index queries.
- **C.** A Redis search or index module, if the competitor definition
  includes it, with its version and configuration pinned.

A client-maintained index is never presented as equal semantics.

## 7. Complexity inventory (G9): counts, not scores

Recorded per system, for the same workload and profile:

- the required services and processes;
- configuration files, and the flags or settings changed from defaults;
- setup commands and steps, from a fresh host to a ready system;
- replication and HA components;
- operator actions for failover and for recovery;
- external dependencies;
- lines and size of the workload's service glue, where meaningful.

Any qualitative judgment is written separately from these counts.

## 8. Order to the freeze

1. Boot the main R640 when convenient.
2. Capture its untouched manifest (section 1).
3. Review the manifest and define the explicit CPU, NUMA, storage and NIC
   allocation (section 5).
4. Implement CP-1A and have it reviewed.
5. Run the SNC-only within-system pilot on the R640, and commit its raw
   results.
6. Implement CP-1B, including the SpacetimeDB workload A harness, and
   have it reviewed.
7. Write the semantics and mismatch table (with the section 6 decision)
   and the complexity inventory.
8. The formal criteria-setting review uses the SNC pilot's evidence to
   choose N_MIN, V_MAX, N, M_B, the Path B thresholds and the p99
   objective.
9. Commit `bench/results/<campaign>/CRITERIA.md`, with exact rules and
   the comparator configurations.
10. Record and freeze the exact SNC SHA.
11. Only then, run the comparative workload A: SNC, PostgreSQL, Redis and
    SpacetimeDB.
12. Later waves: workloads B and C with Orleans, Restate and DBOS, and
    WAN once the remote manifest exists.

Still frozen: Documents, generic Events/Timers, WaitSets, Activation,
the Agent Runtime, snapshot/PITR/branch implementation, security and
capabilities, multi-tenancy, a Rust rewrite, DPDK/VPP, S2/S3, the
recovering acceptor, unrelated optimization.
