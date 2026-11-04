# Experimental validation / falsification milestone

**Status: PLANNED.** It starts after S1 is reviewed and after only the
correctness fixes that the experiments need. Roadmap and scope decision by
cody, 2026-11-04. The source text is
[handoff/SNC_Experimental_Validation_Roadmap.txt](handoff/SNC_Experimental_Validation_Roadmap.txt).

This is the next major milestone. It replaces "keep extending SNC" as the
default plan. **Major platform work after S1 depends on the decision this
milestone ends with** (section 14).

The goal is not to show that SNC is good. It is to run experiments that
could show SNC is **not worth pursuing**. SNC survives only if the evidence
shows a meaningful workload envelope where its value justifies its
complexity.

> The purpose is not to protect SNC. It is to give SNC a serious
> opportunity to fail. STOP is a successful research outcome if evidence
> supports it.

## 1. Why now

The core prototype is already substantial:

- stable ObjectIDs;
- pinned owner-local execution and cross-core dispatch;
- indexes;
- WAL with group commit, checkpoint and recovery;
- durable follow-ups, and cross-shard and cross-node operations;
- replication, witness/Paxos HA and fencing;
- retries;
- placement and migration, with fail-closed migration serving;
- HEALTH decoupling.

None of the following is needed to decide whether the core thesis deserves
more investment: Documents, generic Events/Timers, WaitSets, Activation,
the Agent Runtime, security/capabilities, multi-tenancy, or a Rust rewrite.

## 2. Sequence

1. Do not interrupt S1, the bounded destination staging slice now
   authorized (MIGRATION_STAGING.md).
2. Finish S1 and have it reviewed.
3. Fix only the correctness defects without which the falsification tests
   would be meaningless or unfair. R08 and R11 are likely candidates (see
   EVIDENCE.md section 12); each stays a separately reviewed slice.
4. Freeze major feature expansion (section 3).
5. Establish the **SNC Experimental Baseline**: a fixed commit, its
   machine manifests, and the pre-registered criteria of section 4.
6. Run the falsification program (sections 5-7).
7. Hold a formal **GO / REFINE / NARROW / STOP** review (section 14).
8. Resume major platform expansion only if that decision justifies it.

## 3. Scope freeze after S1

Do not begin any of these merely because they are in the long-term
vision:

- native Documents;
- generic Durable Events/Timers;
- Wait Conditions/WaitSets;
- generic Activation;
- the Agent Runtime;
- an MMO framework;
- a broad plugin/module platform;
- a production security/capability subsystem;
- multi-tenancy;
- a Rust rewrite;
- DPDK/VPP specialization;
- unrelated feature expansion.

**Exception:** work that a validation workload explicitly requires, or the
fix of a defect that would invalidate an experiment. Either goes through
the usual reviewed slice (AGENTS.md).

The reference-architecture research lane ([research/](research/README.md))
continues in parallel.

## 4. Core question and pre-registered criteria

**Does SNC provide enough measurable advantage over simpler or existing
approaches to justify a new state-native execution substrate?**

Ask: *what evidence would convince us SNC should not be built?*

**Rule:** failure criteria are written down and committed **before** the
final comparative results are known, and are not moved afterwards.

**Repository note (how this is applied).** Before the first comparative
run of each workload, a short criteria file is committed. It goes to
`bench/results/<campaign>/CRITERIA.md`, and its commit SHA is quoted in
the results. It names:

- the comparison;
- the matched semantics;
- each metric;
- the threshold that would count against SNC.

Any later change to it is a new commit, explained, and reported next to
the results. No thresholds are proposed here; they are a decision for
cody and Codex at the Experimental Baseline.

**Falsification targets.** Actively try to prove each of these:

1. Owner-local execution has no material realistic advantage.
2. Its benefits disappear under realistic cross-object or cross-core
   coordination.
3. Tail latency becomes unacceptable under skew, migration, failure or
   recovery.
4. Memory or object density is not competitive.
5. WAL, storage or network amplification erases the compute advantage.
6. The complexity of recovery, failover and migration outweighs the
   benefits.
7. Existing systems give similar semantics and performance with much less
   operational complexity.
8. Correctness coordination destroys the locality thesis.
9. SNC scales poorly across cores, NUMA domains or machines.
10. No sufficiently valuable workload envelope exists.

## 5. Validation workloads

### A. Stateful object service

**Setup:**

- millions of durable objects;
- owner-local reads and mutations;
- secondary-index queries;
- configurable Zipf skew;
- configurable cross-object percentage;
- configurable local or remote ownership;
- persistence, replication, and failure/recovery.

**Compare, where semantics can be matched:**

- SNC;
- SpacetimeDB;
- Redis plus a service;
- PostgreSQL plus a service.

**Measure:**

- ops/s per core;
- p50, p95, p99 and p99.9 latency;
- CPU per op and RAM per object;
- WAL/storage bytes per op and network bytes per op;
- recovery and failover interruption;
- operational complexity.

### B. Persistent simulation / MMO-like world

This is a **benchmark workload, not a product pivot.**

**Model:**

- a large persistent entity population;
- spatial/ownership regions;
- high-frequency local interactions and occasional cross-region ones;
- hot and cold regions;
- ownership movement and migration;
- node failure and recovery.

**Compare** mainly SNC, SpacetimeDB and Orleans, where matched semantics
are practical.

**Question:** is natural state locality actually a meaningful SNC
advantage?

### C. Durable workflow / dormant-entity simulation

**Do not build the Agent Runtime for this.** Approximate it with existing
SNC primitives:

```text
many durable entities -> dormant -> wake/event -> read state -> mutate
  -> emit durable obligation -> settle -> dormant
```

Scale toward 100K-1M entities as practical, and inject failure and
recovery.

**Compare** mainly SNC, Restate, and DBOS/PostgreSQL.

**Question:** does SNC provide a meaningful substrate advantage, or do
existing durable-execution systems already provide that value more simply?

### What exists today (repository note, INFERRED from the code and docs)

| Need | Existing | Gap |
| --- | --- | --- |
| A, SNC side | `bench/snc_bench.cpp` (in process), `bench/net_bench.cpp` and `bench/snc_load.cpp` (over TCP, transfers, Zipf, sessions), `bench/lab/lab.py` (multi-host, faults) | Index-query mix and a remote-ownership percentage knob over TCP; RAM per object and bytes per op reported per run |
| A, comparisons | `bench/conventional/` (PostgreSQL 16, Redis 7, ScyllaDB): CONVENTIONAL.md | No SpacetimeDB harness; replication-matched configurations only for PostgreSQL sync streaming |
| B | Primitives: `create_near` (co-location), follow-ups across shards and groups, slot moves | A workload driver (regions as slots, movement as moves); Orleans and SpacetimeDB harnesses |
| C | Primitives: durable follow-ups with `resume` (a durable obligation that is settled), sessions | No timers or dormancy (scope freeze: approximate with an external waker); Restate and DBOS harnesses |
| Locality sweep | CROSSCORE.md and SKEW.md (in-process remote %, Zipf) | NUMA classes and the cross-machine and cross-site classes |
| Failure program | `tests/test_migrate.cpp`, `test_fail_closed.py`, `test_s1.py`, `test_async_failover.py`, lab P3-P6 and soak | Disk-full/EIO injection; randomized long runs combining faults |

## 6. Locality envelope

Sweep the physical cores actually available, after hardware discovery.
**Do not assume the R740xd is available.**

**Example locality sweep (% owner-local):** 100, 95, 90, 75, 50, 25, 0.

**Distinguish, where the hardware permits:**

- the same owner core;
- a different core on the same NUMA node or socket;
- a different NUMA node or socket;
- a different machine;
- a remote physical site.

Measure throughput and tail latency to derive the actual SNC
locality/coordination performance surface.

## 7. Lab topology

- **Main local SNC server: the user's second server, not the R740xd.**
  Known approximately: about 384 GB RAM, Intel Xeon Gold processors, exact
  CPU model and core count unknown. It is available for bare-metal Linux
  SNC testing.
- **R740xd: no bare-metal SNC testing.** It runs TrueNAS Scale as its
  bare-metal OS and is not the primary benchmark host.
- **Remote physical-site host:** a Linux machine at another physical
  location. Record its hardware and network manifest before use. Use it
  for real WAN and cross-site testing:
  - RTT and jitter;
  - replication;
  - cross-node operations;
  - interruption and reconnect;
  - failover assumptions;
  - cross-site migration where sensible;
  - bandwidth amplification;
  - tail latency.

  WAN results and LAN/local results are **separate experimental classes**.
- **Cloud/VPS:** keep disposable VPS hosts for reproducible fault
  injection. They complement the physical-site tests; they do not replace
  them.
- **This project's cloud VM** (4 vCPU, `bench/manifests/dev-vm-4vcpu-20261104.txt`).
  It produced every loopback number so far. It is a development host,
  not an experiment host.
  - Multi-process runs on it are CPU-oversubscribed: 8 server processes
    with 16 spinning shard threads, plus witnesses and probes, on 4
    vCPUs.
  - That changes wall-clock results. The S1 measurements show it
    (MIGRATION_STAGING.md section 18).
  - Its numbers are not used in the GO/STOP evidence except as
    regression checks.

## 8. Machine manifests (required)

Before benchmarking on any host, record its manifest with
`bench/manifest.sh NAME DATA_DIR NIC...` and commit it as
`bench/manifests/NAME-YYYYMMDD.txt` with the benchmark evidence. **Do not
guess CPU topology.**

The script records:

- `lscpu`, `lscpu -e`, `numactl --hardware`;
- `free -h`, `lsblk`, `lspci -nn`;
- `ethtool` for each test NIC;
- the kernel version, governor and THP setting;
- the data directory's filesystem and mount options.

Manifests on record (bench/manifests/README.md):

| Host | Manifest |
| --- | --- |
| This project's cloud VM | `dev-vm-4vcpu-20261104.txt` |
| Vultr VPSs of 2026-11-04 | Not captured by the script; the hosts are destroyed. What was recorded is in REAL_MACHINES.md "Vultr run" |
| Main local server (second server) | **To capture before its first run** |
| Remote physical-site host | **To capture before its first run** |
| Any new VPS | To capture at creation |

## 9. Benchmark discipline

**Match semantics wherever possible.** Never compare SNC durable and
replicated operations against a competitor's volatile or local operations
and call it a win.

**For each comparison, record:**

- the durability guarantee, replication guarantee and acknowledgement
  point;
- the consistency model;
- operation semantics and index semantics;
- batching/pipelining and client concurrency;
- the hardware allocation, storage and network;
- the software version or commit, and the configuration.

If exact matching is impossible, state the mismatch prominently.

**Raw results** go under `bench/results/<campaign>/`, with:

- the exact command lines and configuration;
- the SNC commit SHA (and competitors' versions);
- the manifest names of every host used.

### Correctness before performance

A benchmark is invalid if the tested configuration violates its declared
contract. Do not benchmark around R08, R11 or another known issue while
presenting stronger semantics than the configuration provides.

For example:

- An async-replicated SNC group is reported as "acked writes may be lost
  on failover" (R08).
- A run whose sessions can expire before a resend is not presented as
  exactly-once (R11).

## 10. Failure / adversarial program

Exercise combinations of:

- **process faults:** SIGKILL, SIGSTOP/stalls, resets;
- **network faults:** silent or asymmetric partitions, reconnect/flap;
- **replica faults:** standby loss, primary loss;
- **failover under load,** during a retry, and during a migration;
- **migration faults:** driver death, migration restart;
- **load shapes:** overload, hot-key skew, poor locality, a high
  cross-owner percentage;
- **recovery** under load, and its interaction with checkpoints;
- **storage faults:** disk-full/EIO where safely injectable, and WAL
  corruption characterization.

Use deterministic regressions, plus longer randomized or adversarial runs
where practical.

## 11. Scale ladder

Objects or entities: 100K, 250K, 500K, 1M, 5M, 10M. Continue until a
meaningful resource or system limit is reached.

Record which limit it was: memory, CPU, storage, network, recovery time,
latency, correctness, or operational impracticality. **A failure to scale
is evidence.**

## 12. Metrics

At minimum:

- **Throughput and latency:** throughput; p50, p95, p99 and p99.9.
- **Cost:** CPU per op; memory (bytes per object).
- **I/O:** WAL bytes per op; storage IOPS and bandwidth; network bytes per
  op; cross-core and cross-node traffic where measurable.
- **Disruption:** recovery; failover interruption; migration total time
  and client-visible interruption.
- **Load state:** replication lag; queue/backlog high-water.
- **Client outcomes:** errors, refusals and retries.

**Operational complexity:**

- the number of services and processes;
- the configuration surface;
- deployment steps;
- recovery and operator actions;
- manual-intervention failure modes.

## 13. Reference research lane

Continue in parallel, using these systems as adversarial design reviewers,
not as feature shopping lists ([research/README.md](research/README.md)):

1. **Styx:** migration, acknowledgement, deterministic execution.
2. **Restate:** scheduler, backpressure, dormancy.
3. **Orleans:** identity, activation, locality, reminders.
4. **SpacetimeDB:** integrated state/computation and transactions.
5. **DBOS:** Postgres durable execution; the null hypothesis.

Their reviews feed the design of the comparative workloads (section 5):
which semantics can be matched, and how each system is configured fairly.

## 14. GO / REFINE / NARROW / STOP

- **GO:** evidence shows a meaningful, defensible workload envelope with
  sufficient performance, density, predictability, failure semantics or
  operational simplicity.
- **REFINE:** the thesis is promising, but a small number of measured
  architectural bottlenecks prevent a fair conclusion. Do only bounded
  work on those bottlenecks, then rerun.
- **NARROW:** SNC is valuable only for a narrower class. Narrow the
  architecture and product rather than pretending it is general.
- **STOP:** existing approaches are equivalent, better or simpler; SNC's
  advantages disappear under realistic coordination or failure; or the
  complexity cannot be justified.

Only this decision reopens the work frozen in section 3. If SNC survives
and shows a meaningful advantage, that evidence is what justifies building
the rest of the platform.
